/** @file
  M3Early (STARBITE_M3_EARLY=1): docs/WIN11-M3-CLEAN.md section 9. The boot CPU moves to M3 core 1 at ReadyToBoot,
  BEFORE the boot manager and winload run, so every boot-CPU decision from then on is made on an M3. LOG ONLY: at
  ExitBootServices (on the M3) the image logs and resets; Windows' kernel never starts. Nothing of Windows is patched.

  Dispatch: M3 core 1 is CPU_ON'd once into a parking loop (MMU off, WFE, polls a Go word), while PSCI still goes
  straight to Samsung's EL3 (the woa shim installs later, in BDS). The six PMU defaults first, as every M3 start.

  ReadyToBoot (our callback, TPL raised to HIGH so no tick fires during the move):
    1. the A55's banked GIC state (ISENABLER0, IPRIORITYR0..7) and CPU interface (CTLR, PMR, BPR) and its timers
       (CNTP/CNTV CTL+CVAL) are saved;
    2. the full EL1 context goes to the M3 (the old M3Boot's mechanism, proven in solo3..solo10): Go word, the M3
       restores MMU/caches/VBAR/SPs/registers and returns from the hand-off function in our callback;
    3. on the M3: the banked GIC state and CPU interface are written to its own interface, every SPI that targeted
       A55 0 (interface 0) is retargeted to interface 5, the timers are restored; TPL back: UEFI's tick now runs on
       the M3, checked with a 10 ms timer event;
    4. A55 0 masks its timers and becomes a watcher on a stack of its own: it rewrites one log line ("=M3E watch")
       in both RAM rings 2x a second for 90 s (beat, the M3's progress, the pmsg write index), then parks in WFI.
  ExitBootServices (our wrapper, outermost): logs the CPU it runs on, calls the real one (woa's hook included), logs
  the live tables' state and resets (ResetSystem, then the FADT reset register 0x14060400 = 1).
  Fail-safe: if the M3 doesn't claim within 1 s the A55 stays the boot CPU (and still resets at ExitBootServices).

  Copyright (c) 2026, Starbite. SPDX-License-Identifier: GPL-2.0-only
**/

#include <PiDxe.h>
#include <IndustryStandard/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Guid/EventGroup.h>

#ifndef STARBITE_M3_EARLY
#define STARBITE_M3_EARLY 0
#endif

#if STARBITE_M3_EARLY

#include "StarbiteLog.h"

#define E_GICD          0x10101000UL
#define E_GICC          0x10102000UL
#define E_TARGET        0x101u               // M3 core 1
#define E_IFACE_BIT     0x20u                // its GIC CPU interface (4 + 1), B3a
#define E_TIMEOUT       26000000u            // 1 s of the 26 MHz counter
#define E_WATCH_TICKS   (26000000ull * 90)

typedef struct {
  UINT64    X[12];                                   // 0x000 x19..x30
  UINT64    SpEl0, SpEl1, SpSel, Daif, Nzcv, Fpcr, Fpsr;            // 0x060
  UINT64    Sctlr, Tcr, Ttbr0, Ttbr1, Mair, Amair, Vbar, Cpacr;     // 0x098
  UINT64    ContextId, Tpidr0, Tpidr1, TpidrRo, Cntkctl;            // 0x0d8
  UINT32    State, Stage;                                           // 0x100
  UINT64    El, Mpidr, Timeout, Rc, Esr, Elr, Far;                  // 0x108
  UINT64    Q[64];                                                  // 0x140 q0..q31
  UINT64    Pad[8];                                                 // 0x340: [0] x18, [1] progress, [3] watch sp
  UINT64    DStage, DEl, DMpidr, DEsr, DElr, DFar;                  // 0x380 (the M3's own line)
} M3E_CTX;

STATIC_ASSERT (OFFSET_OF (M3E_CTX, SpEl0) == 0x060, "SP_EL0");
STATIC_ASSERT (OFFSET_OF (M3E_CTX, Sctlr) == 0x098, "SCTLR");
STATIC_ASSERT (OFFSET_OF (M3E_CTX, Cntkctl) == 0x0f8, "CNTKCTL");
STATIC_ASSERT (OFFSET_OF (M3E_CTX, State) == 0x100, "STATE");
STATIC_ASSERT (OFFSET_OF (M3E_CTX, Timeout) == 0x118, "TIMEOUT");
STATIC_ASSERT (OFFSET_OF (M3E_CTX, Rc) == 0x120, "RC");
STATIC_ASSERT (OFFSET_OF (M3E_CTX, Q) == 0x140, "Q0");
STATIC_ASSERT (OFFSET_OF (M3E_CTX, Pad) == 0x340, "PAD");
STATIC_ASSERT (OFFSET_OF (M3E_CTX, DStage) == 0x380, "DSTAGE");
STATIC_ASSERT (sizeof (M3E_CTX) <= 0x400, "size");

UINT64  gM3EarlyCtx __attribute__ ((used));

extern UINT8  M3EarlyVectors[];
extern UINT8  M3EarlyEntry[];
extern UINT8  M3EarlyEnd[];

//
// The M3's resume path (in this image's .text, as M3Boot v2+): MMU off on entry from the park loop.
//
__asm__ (
  "  .text\n"
  "  .p2align 11\n"
  "  .globl M3EarlyVectors\n"
  "M3EarlyVectors:\n"
  "  .rept 16\n"
  "  b     .Lm3e_vfail\n"
  "  .p2align 7\n"
  "  .endr\n"
  "  .globl M3EarlyEntry\n"
  "M3EarlyEntry:\n"
  "  adrp  x1, gM3EarlyCtx\n"
  "  ldr   x19, [x1, :lo12:gM3EarlyCtx]\n"
  "  mov   x0, #1\n"
  "  str   x0, [x19, #0x380]\n"
  "  mrs   x0, CurrentEL\n"
  "  lsr   x0, x0, #2\n"
  "  str   x0, [x19, #0x388]\n"
  "  mrs   x2, mpidr_el1\n"
  "  str   x2, [x19, #0x390]\n"
  "  dsb   sy\n"
  "  cmp   x0, #1\n"
  "  b.ne  .Lm3e_park\n"
  "  tlbi  vmalle1\n"
  "  dsb   nsh\n"
  "  ic    iallu\n"
  "  dsb   nsh\n"
  "  isb\n"
  "  adr   x0, M3EarlyVectors\n"
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
  "  str   x0, [x19, #0x380]\n"
  "  mrs   x0, mpidr_el1\n"
  "  str   x0, [x19, #0x390]\n"
  "  add   x2, x19, #0x100\n"            // claim: State 0 -> 1 (2 = the A55 gave up: stay parked)
  "1:\n"
  "  ldaxr w0, [x2]\n"
  "  cbnz  w0, .Lm3e_park\n"
  "  mov   w1, #1\n"
  "  stlxr w3, w1, [x2]\n"
  "  cbnz  w3, 1b\n"
  "  dmb   ish\n"
  "  mov   x0, #3\n"
  "  str   x0, [x19, #0x380]\n"
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
  ".Lm3e_vfail:\n"                       // an exception before the claim: record, then stay parked
  "  adrp  x1, gM3EarlyCtx\n"
  "  ldr   x19, [x1, :lo12:gM3EarlyCtx]\n"
  "  mrs   x0, esr_el1\n  str x0, [x19, #0x398]\n"
  "  mrs   x0, elr_el1\n  str x0, [x19, #0x3a0]\n"
  "  mrs   x0, far_el1\n  str x0, [x19, #0x3a8]\n"
  "  dsb   sy\n"
  ".Lm3e_park:\n"                        // no CPU_OFF (woa: EL3 hung the platform after CPU_OFFs)
  "2:\n"
  "  wfe\n"
  "  b     2b\n"
  "  .globl M3EarlyEnd\n"
  "M3EarlyEnd:\n"
  );

//
// The parking loop M3 core 1 runs from dispatch to ReadyToBoot (M3Boot v3's): reserved page, +0x800, literals (the
// page, M3EarlyEntry). +0x80 crumb (1 parked, 2 left), +0x88 MPIDR; event stream on; WFE until +0x40 Go == 1.
//
extern UINT8  M3EarlyPark[];
extern UINT8  M3EarlyParkLit[];
extern UINT8  M3EarlyParkEnd[];

__asm__ (
  "  .text\n"
  "  .p2align 3\n"
  "  .globl M3EarlyPark\n"
  "M3EarlyPark:\n"
  "  adr   x20, .Lm3e_parklit\n"
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
  "  .globl M3EarlyParkLit\n"
  "M3EarlyParkLit:\n"
  ".Lm3e_parklit:\n"
  "  .quad 0\n"
  "  .quad 0\n"
  "  .globl M3EarlyParkEnd\n"
  "M3EarlyParkEnd:\n"
  );

STATIC CONST struct {
  UINT32    Offset, Mask, Value;
} mEPmu[] = {
  { 0x1088, 0xFF000000u, 0x8D000000u },
  { 0x2114, 3, 2 }, { 0x2180, 3, 2 }, { 0x21EC, 3, 2 }, { 0x2258, 3, 2 }, { 0x2384, 3, 2 },
};

STATIC EFI_GUID  mEAcpi20Guid = {
  0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 }
};

STATIC M3E_CTX                 *mECtx;
STATIC EFI_PHYSICAL_ADDRESS    mEPark;
STATIC EFI_PHYSICAL_ADDRESS    mEWatchStack;
STATIC EFI_EXIT_BOOT_SERVICES  mEOrigEbs;
STATIC EFI_EVENT               mEReadyToBoot;
STATIC BOOLEAN                 mEMoved;
STATIC BOOLEAN                 mEFired;
STATIC UINT32                  mEEn0, mEPri[8], mECtlr, mEPmr, mEBpr;
STATIC UINT64                  mEPctl, mEPcval, mEVctl, mEVcval;
STATIC UINTN                   mEWatch[2][4];      // [ring][field] first digit: beat prog pmsg; [3] the line's start
STATIC CONST CHAR8             mEWatchTag[] = "=M3E watch beat ";

STATIC
UINT64
ESmc (
  IN UINT64  Fn,
  IN UINT64  A1,
  IN UINT64  A2
  )
{
  register UINT64  x0 __asm__ ("x0") = Fn;
  register UINT64  x1 __asm__ ("x1") = A1;
  register UINT64  x2 __asm__ ("x2") = A2;
  register UINT64  x3 __asm__ ("x3") = 0;

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
EClean (
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
UINT64
EMpidr (
  VOID
  )
{
  UINT64  M;

  __asm__ __volatile__ ("mrs %0, mpidr_el1" : "=r" (M));
  return M;
}

//
// Progress of the M3 (read by the A55's watch): 1 claimed, 2 GIC/timer moved, 3 tick seen, 4 leaving the
// ReadyToBoot callback, 5 ExitBootServices wrapper entered, 6 the real ExitBootServices returned.
//
STATIC
VOID
EProgress (
  IN UINT64  P
  )
{
  if ((mECtx == NULL) || ((EMpidr () & 0xFFFFFF) != E_TARGET)) {
    return;
  }

  mECtx->Pad[1] = P;
  EClean ((UINTN)&mECtx->Pad[1], (UINTN)&mECtx->Pad[1] + 8);
}

STATIC
VOID
EHex (
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
    d                           = (UINT8)(V & 0xF);
    *(volatile UINT8 *)(At + i) = (UINT8)((d < 10) ? ('0' + d) : ('A' + d - 10));
    V                         >>= 4;
  }
}

//
// Needle's first character in a ring (data at Base + 12, Cap bytes), searched over the whole data area.
//
STATIC
UINTN
ERingFind (
  IN UINTN        Base,
  IN UINTN        Cap,
  IN CONST CHAR8  *Needle
  )
{
  volatile UINT8  *B = (volatile UINT8 *)(Base + 12);
  UINTN           Len;
  UINTN           Pos;
  UINTN           i;

  if (*(volatile UINT32 *)Base != SB_DBGC) {
    return 0;
  }

  Len = AsciiStrLen (Needle);
  for (Pos = Cap - Len + 1; Pos-- > 0;) {
    for (i = 0; (i < Len) && (B[Pos + i] == (UINT8)Needle[i]); i++) {
    }

    if (i == Len) {
      return Base + 12 + Pos;
    }
  }

  return 0;
}

VOID
M3EarlyA55Watch (
  M3E_CTX  *Ctx
  ) __attribute__ ((used, noreturn));

VOID
M3EarlyA55Watch (
  M3E_CTX  *Ctx
  )
{
  UINT64  T0, Now, Last, Sctlr;
  UINT32  Beat;
  UINTN   r;

  __asm__ __volatile__ ("mrs %0, cntvct_el0" : "=r" (T0));
  Last = T0;
  for (Beat = 0; ; ) {
    __asm__ __volatile__ ("isb\n mrs %0, cntvct_el0" : "=r" (Now));
    if (Now - Last < 13000000u) {       // 0.5 s
      continue;
    }

    Last = Now;
    Beat++;
    EClean ((UINTN)&Ctx->Pad[0], (UINTN)Ctx + 0x400);
    EClean (SB_PMSG_BASE, SB_PMSG_BASE + 16);
    for (r = 0; r < 2; r++) {
      if ((mEWatch[r][3] == 0) || (CompareMem ((VOID *)mEWatch[r][3], mEWatchTag, sizeof (mEWatchTag) - 1) != 0)) {
        continue;                       // the ring wrapped over our line: leave whatever is there now
      }

      EHex (mEWatch[r][0], Beat, 4);
      EHex (mEWatch[r][1], Ctx->Pad[1], 2);
      EHex (mEWatch[r][2], *(volatile UINT32 *)(SB_PMSG_BASE + 8), 4);
    }

    EClean (SB_EVLOG_BASE, SB_EVLOG_BASE + 12 + SB_EVLOG_CAP);
    EClean (SB_PMSG_BASE, SB_PMSG_BASE + 12 + SB_PMSG_CAP);
    if (Now - T0 > E_WATCH_TICKS) {
      break;
    }
  }

  //
  // Park in WFI in the reserved stack page with MMU and caches off (M3Boot v7: stable; no CPU_OFF).
  //
  __asm__ __volatile__ ("mrs %0, sctlr_el1" : "=r" (Sctlr));
  Sctlr &= ~(UINT64)((1u << 0) | (1u << 2) | (1u << 12));
  __asm__ __volatile__ (
    "dsb sy\n msr sctlr_el1, %0\n isb\n ic iallu\n dsb sy\n isb\n br %1\n"
    :
    : "r" (Sctlr), "r" ((UINT64)mEWatchStack)
    : "memory"
    );
  for ( ; ;) {
  }
}

//
// A55 side (M3Boot v3's hand-off, with the claim path changed): x0 = context, x1 = the Go word. Returns 1 on the M3,
// 0 on the A55 if the M3 didn't claim within Ctx->Timeout ticks. After a claim the A55 masks its own timers and
// becomes the watcher on its own stack (Pad[3]); it never returns.
//
STATIC
__attribute__ ((naked))
UINT64
M3EarlyHandoff (
  IN M3E_CTX  *Ctx,
  IN UINT64   GoAddr
  )
{
  __asm__ __volatile__ (
    "stp   x19, x20, [x0, #0x00]\n"
    "stp   x21, x22, [x0, #0x10]\n"
    "stp   x23, x24, [x0, #0x20]\n"
    "stp   x25, x26, [x0, #0x30]\n"
    "stp   x27, x28, [x0, #0x40]\n"
    "stp   x29, x30, [x0, #0x50]\n"
    "str   x18, [x0, #0x340]\n"
    "mov   x9, sp\n"
    "mrs   x10, spsel\n"
    "str   x10, [x0, #0x70]\n"
    "cbz   x10, 1f\n"
    "str   x9, [x0, #0x68]\n"
    "mrs   x11, sp_el0\n"
    "str   x11, [x0, #0x60]\n"
    "b     2f\n"
    "1:\n"
    "str   x9, [x0, #0x60]\n"
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
    "str   wzr, [x0, #0x100]\n"
    "mov   x9, x0\n"
    "add   x10, x0, #0x400\n"
    "3:\n"
    "dc    civac, x9\n"
    "add   x9, x9, #64\n"
    "cmp   x9, x10\n"
    "b.lo  3b\n"
    "dsb   sy\n"
    "mov   x19, x0\n"
    "mov   w9, #1\n"
    "str   w9, [x1]\n"                    // Go
    "dc    civac, x1\n"
    "dsb   sy\n"
    "sev\n"
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
    "5:\n"
    "ldaxr w9, [x22]\n"
    "cbnz  w9, 6f\n"
    "mov   w10, #2\n"
    "stlxr w11, w10, [x22]\n"
    "cbnz  w11, 5b\n"
    "b     7f\n"
    "6:\n"                                // claimed: this A55 stops being the boot CPU
    "msr   cntp_ctl_el0, xzr\n"
    "msr   cntv_ctl_el0, xzr\n"
    "isb\n"
    "ldr   x9, [x19, #0x358]\n"
    "mov   sp, x9\n"
    "mov   x0, x19\n"
    "bl    M3EarlyA55Watch\n"
    "8:\n"
    "wfe\n"
    "b     8b\n"
    "7:\n"                                // not claimed: stay the boot CPU
    "str   wzr, [x1]\n"
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
VOID
ESaveA55Gic (
  VOID
  )
{
  volatile UINT32  *D = (volatile UINT32 *)E_GICD;
  volatile UINT32  *C = (volatile UINT32 *)E_GICC;
  UINTN            i;

  mEEn0 = D[0x100 / 4];
  for (i = 0; i < 8; i++) {
    mEPri[i] = D[0x400 / 4 + i];
  }

  mECtlr = C[0];
  mEPmr  = C[1];
  mEBpr  = C[2];
  __asm__ __volatile__ ("mrs %0, cntp_ctl_el0" : "=r" (mEPctl));
  __asm__ __volatile__ ("mrs %0, cntp_cval_el0" : "=r" (mEPcval));
  __asm__ __volatile__ ("mrs %0, cntv_ctl_el0" : "=r" (mEVctl));
  __asm__ __volatile__ ("mrs %0, cntv_cval_el0" : "=r" (mEVcval));
}

//
// On the M3: its own banked distributor registers and CPU interface as the A55 had them, SPIs aimed at interface 0
// moved to the M3's interface, the timers restored (CVAL is absolute; CNTVCT and CNTPCT are shared).
//
STATIC
UINTN
EApplyGicOnM3 (
  VOID
  )
{
  volatile UINT32  *D = (volatile UINT32 *)E_GICD;
  volatile UINT32  *C = (volatile UINT32 *)E_GICC;
  volatile UINT8   *T = (volatile UINT8 *)(E_GICD + 0x800);
  UINTN            i;
  UINTN            Lines;
  UINTN            Moved;
  UINT8            b;

  for (i = 0; i < 8; i++) {
    D[0x400 / 4 + i] = mEPri[i];
  }

  D[0x100 / 4] = mEEn0;                  // write-1-to-set, banked
  C[1]         = mEPmr;
  C[2]         = mEBpr;
  C[0]         = mECtlr;
  Lines        = ((D[1] & 0x1F) + 1) * 32;
  Moved        = 0;
  for (i = 32; i < Lines; i++) {
    b = T[i];
    if ((b & 1) != 0) {
      T[i] = (UINT8)((b & ~1u) | E_IFACE_BIT);
      Moved++;
    }
  }

  __asm__ __volatile__ ("msr cntp_cval_el0, %0\n msr cntp_ctl_el0, %1\n isb" : : "r" (mEPcval), "r" (mEPctl & 1) : "memory");
  __asm__ __volatile__ ("msr cntv_cval_el0, %0\n msr cntv_ctl_el0, %1\n isb" : : "r" (mEVcval), "r" (mEVctl & 1) : "memory");
  return Moved;
}

STATIC
VOID
EAcpiLine (
  IN CONST CHAR8  *Tag
  )
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp;
  EFI_ACPI_DESCRIPTION_HEADER                   *X;
  EFI_ACPI_DESCRIPTION_HEADER                   *T;
  UINT64                                        E;
  UINTN                                         i;
  UINT16                                        Ba;
  UINT32                                        Gicc;

  Rsdp = NULL;
  for (i = 0; i < gST->NumberOfTableEntries; i++) {
    if (CompareGuid (&gST->ConfigurationTable[i].VendorGuid, &mEAcpi20Guid)) {
      Rsdp = gST->ConfigurationTable[i].VendorTable;
    }
  }

  if ((Rsdp == NULL) || (Rsdp->XsdtAddress == 0)) {
    SbLog ("=M3E acpi %a: none", Tag);
    return;
  }

  X    = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
  Ba   = 0xFFFF;
  Gicc = 0;
  for (i = 0; i < (X->Length - sizeof (*X)) / 8; i++) {
    CopyMem (&E, (UINT8 *)(X + 1) + 8 * i, 8);
    T = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)E;
    if (T == NULL) {
      continue;
    }

    if (T->Signature == SIGNATURE_32 ('F', 'A', 'C', 'P')) {
      CopyMem (&Ba, (UINT8 *)T + 129, 2);
    } else if (T->Signature == SIGNATURE_32 ('A', 'P', 'I', 'C')) {
      UINT8   *M = (UINT8 *)T;
      UINT32  Off;
      UINT32  Fl;

      for (Off = 44; Off + 2 <= T->Length && M[Off + 1] != 0; Off += M[Off + 1]) {
        if (M[Off] == 0x0B) {
          CopyMem (&Fl, M + Off + 12, 4);
          Gicc = (Gicc << 1) | (Fl & 1);
        }
      }
    }
  }

  SbLog ("=M3E acpi %a: ARM_BOOT_ARCH %x, MADT GICC enabled bits (UID 0 first) %x", Tag, Ba, Gicc);
}

STATIC
EFI_STATUS
EFIAPI
M3EarlyEbsWrap (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MapKey
  )
{
  EFI_STATUS  Status;

  if (!mEFired) {
    mEFired = TRUE;
    EProgress (5);
    SbLog ("=M3E ExitBootServices called from %lx, running on mpidr %lx (80000101 = the M3)", (UINT64)(UINTN)__builtin_return_address (0), EMpidr ());
    EAcpiLine ("ebs-pre");
  }

  Status = mEOrigEbs (ImageHandle, MapKey);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mSbAfterEbs = TRUE;
  EProgress (6);
  SbLog ("=M3E ExitBootServices returned %r on mpidr %lx (woa's hook included)", Status, EMpidr ());
  EAcpiLine ("ebs-post");
  SbLog ("=M3E v1 done: UEFI, the boot manager and winload ran on %a. Windows' kernel was NOT started. Resetting", mEMoved ? "M3 core 1" : "A55 core 0 (hand-off not taken)");
  gST->RuntimeServices->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);
  SbLog ("=M3E ResetSystem returned; FADT reset register (PMU SWRESET) now");
  *(volatile UINT32 *)0x14060400UL = 1;
  for ( ; ;) {
    __asm__ __volatile__ ("msr daifset, #0xf\n wfi" ::: "memory");
  }

  return Status;
}

STATIC
VOID
EWatchLine (
  VOID
  )
{
  STATIC CONST CHAR8  Line[]    = "=M3E watch beat 0000 prog 00 pmsg 0000 (A55 0 rewrites this line; prog 4 = left ReadyToBoot, 5 = in EBS, 6 = EBS done)";
  STATIC CONST CHAR8  *Field[3] = { " beat ", " prog ", " pmsg " };
  UINTN               Base[2];
  UINTN               r;
  UINTN               k;
  CHAR8               *Hit;

  SbLog ("%a", Line);
  Base[0] = ERingFind (SB_EVLOG_BASE, SB_EVLOG_CAP, "=M3E watch beat 0000 prog 00");
  Base[1] = ERingFind (SB_PMSG_BASE, SB_PMSG_CAP, "=M3E watch beat 0000 prog 00");
  for (r = 0; r < 2; r++) {
    for (k = 0; k < 3; k++) {
      Hit           = AsciiStrStr (Line, Field[k]);
      mEWatch[r][k] = (Base[r] != 0) ? Base[r] + (UINTN)(Hit - Line) + AsciiStrLen (Field[k]) : 0;
    }

    mEWatch[r][3] = Base[r];
  }

  SbLog ("=M3E watch line found: zone0 %lx pmsg %lx", (UINT64)Base[0], (UINT64)Base[1]);
}

STATIC
VOID
EFIAPI
M3EarlyOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_TPL     Old;
  EFI_STATUS  Status;
  EFI_EVENT   Tick;
  UINT64      Ret;
  UINTN       Moved;
  UINTN       Ms;
  UINT64      Daif;

  if (gBS->ExitBootServices == M3EarlyEbsWrap) {
    return;
  }

  mEOrigEbs             = gBS->ExitBootServices;
  gBS->ExitBootServices = M3EarlyEbsWrap;
  gBS->Hdr.CRC32        = 0;
  gBS->CalculateCrc32 ((UINT8 *)gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
  EAcpiLine ("ready-to-boot");
  EClean ((UINTN)mEPark + 0x80, (UINTN)mEPark + 0x90);
  SbLog (
    "=M3E ReadyToBoot: EBS wrapped; A55 PSCI_VERSION %lx; parked M3 crumb %ld mpidr %lx; moving the boot CPU now",
    ESmc (0x84000000u, 0, 0) & 0xFFFFFFFFu,
    *(volatile UINT64 *)(UINTN)(mEPark + 0x80),
    *(volatile UINT64 *)(UINTN)(mEPark + 0x88)
    );
  if (*(volatile UINT64 *)(UINTN)(mEPark + 0x80) != 1) {
    SbLog ("=M3E: M3 core 1 is not parked; staying on A55 0");
    return;
  }

  EWatchLine ();
  Old = gBS->RaiseTPL (TPL_HIGH_LEVEL);
  ESaveA55Gic ();
  ZeroMem (mECtx, sizeof (*mECtx));
  mECtx->Timeout = E_TIMEOUT;
  mECtx->Pad[3]  = (UINT64)(mEWatchStack + EFI_PAGE_SIZE - 16);
  gM3EarlyCtx    = (UINT64)(UINTN)mECtx;
  EClean ((UINTN)M3EarlyVectors, (UINTN)M3EarlyEnd);
  EClean ((UINTN)&gM3EarlyCtx, (UINTN)&gM3EarlyCtx + 8);
  __asm__ __volatile__ ("ic iallu\n dsb sy\n isb" ::: "memory");
  Ret = M3EarlyHandoff (mECtx, (UINT64)(UINTN)(mEPark + 0x40));
  if (Ret != 1) {
    gBS->RestoreTPL (Old);
    EClean ((UINTN)mECtx + 0x380, (UINTN)mECtx + 0x3B0);
    SbLog ("=M3E: the M3 did not claim within 1 s (stage %ld el %ld mpidr %lx esr %lx elr %lx); staying on A55 0", mECtx->DStage, mECtx->DEl, mECtx->DMpidr, mECtx->DEsr, mECtx->DElr);
    return;
  }

  //
  // On M3 core 1 now.
  //
  mEMoved = TRUE;
  EProgress (1);
  Moved = EApplyGicOnM3 ();
  EProgress (2);
  gBS->RestoreTPL (Old);
  __asm__ __volatile__ ("mrs %0, daif" : "=r" (Daif));
  SbLog (
    "=M3E now on mpidr %lx (el %ld): A55 GIC en0 %x pri7 %x ctlr %x pmr %x bpr %x, %d SPIs moved to if 5; timers p %lx/%lx v %lx/%lx; daif %lx",
    EMpidr (), mECtx->DEl, mEEn0, mEPri[7], mECtlr, mEPmr, mEBpr, Moved, mEPctl, mEPcval, mEVctl, mEVcval, Daif
    );
  Tick   = NULL;
  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &Tick);
  if (!EFI_ERROR (Status)) {
    Status = gBS->SetTimer (Tick, TimerRelative, 100000);      // 10 ms
  }

  for (Ms = 0; Ms < 500 && !EFI_ERROR (Status); Ms++) {
    if (gBS->CheckEvent (Tick) == EFI_SUCCESS) {
      break;
    }

    MicroSecondDelay (1000);
  }

  if (Tick != NULL) {
    gBS->CloseEvent (Tick);
  }

  SbLog (
    "=M3E UEFI tick on the M3: a 10 ms timer event %a after %d ms (%r); gicc hppir %x",
    (Ms < 500 && !EFI_ERROR (Status)) ? "FIRED" : "did NOT fire",
    Ms,
    Status,
    *(volatile UINT32 *)(E_GICC + 0x18)
    );
  EProgress ((Ms < 500) ? 3 : 0x33);
  EProgress (4);
}

VOID
M3EarlyArm (
  VOID
  )
{
  volatile UINT32       *Pmu = (volatile UINT32 *)0x14060000UL;
  EFI_PHYSICAL_ADDRESS  Page;
  EFI_STATUS            Status;
  UINT64                Ver;
  UINT64                Rc;
  UINT64                *Lit;
  UINTN                 i;
  UINTN                 Ms;

  SbLog ("=M3E v1 armed: log only; boot CPU -> M3 core 1 at ReadyToBoot (before winload), reset at ExitBootServices");
  if (((UINTN)M3EarlyVectors & 0x7FF) != 0) {
    SbLog ("=M3E: vectors %lx not 2 KiB aligned, not armed", (UINT64)(UINTN)M3EarlyVectors);
    return;
  }

  Ver = ESmc (0x84000000u, 0, 0) & 0xFFFFFFFFu;
  if (Ver != 0xFFFFFFFFu) {
    SbLog ("=M3E: PSCI_VERSION %lx at dispatch (shim already in), not armed", Ver);
    return;
  }

  Page = 0xFFFFFFFFu;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, 3, &Page))) {
    SbLog ("=M3E: no pages, not armed");
    return;
  }

  ZeroMem ((VOID *)(UINTN)Page, 3 * EFI_PAGE_SIZE);
  mECtx        = (M3E_CTX *)(UINTN)Page;
  mEPark       = Page + EFI_PAGE_SIZE;
  mEWatchStack = Page + 2 * EFI_PAGE_SIZE;
  ((UINT32 *)(UINTN)mEWatchStack)[0] = 0xD503207Fu;           // WFI
  ((UINT32 *)(UINTN)mEWatchStack)[1] = 0x17FFFFFFu;           // B .-4
  CopyMem ((VOID *)(UINTN)(mEPark + 0x800), M3EarlyPark, (UINTN)(M3EarlyParkEnd - M3EarlyPark));
  Lit    = (UINT64 *)(UINTN)(mEPark + 0x800 + (UINTN)(M3EarlyParkLit - M3EarlyPark));
  Lit[0] = mEPark;
  Lit[1] = (UINT64)(UINTN)M3EarlyEntry;
  EClean ((UINTN)Page, (UINTN)Page + 3 * EFI_PAGE_SIZE);
  __asm__ __volatile__ ("ic iallu\n dsb sy\n isb" ::: "memory");
  for (i = 0; i < ARRAY_SIZE (mEPmu); i++) {
    Pmu[mEPmu[i].Offset / 4] = (Pmu[mEPmu[i].Offset / 4] & ~mEPmu[i].Mask) | mEPmu[i].Value;
    __asm__ __volatile__ ("dsb sy" ::: "memory");
  }

  Rc = ESmc (0xC4000003u, E_TARGET, mEPark + 0x800);
  for (Ms = 0; Ms < 200; Ms++) {
    EClean ((UINTN)mEPark + 0x80, (UINTN)mEPark + 0x90);
    if (*(volatile UINT64 *)(UINTN)(mEPark + 0x80) == 1) {
      break;
    }

    MicroSecondDelay (1000);
  }

  SbLog (
    "=M3E M3 core 1 parked at dispatch: CPU_ON rc=%lx crumb %ld after %d ms mpidr %lx; context %lx entry %lx",
    Rc,
    *(volatile UINT64 *)(UINTN)(mEPark + 0x80),
    Ms,
    *(volatile UINT64 *)(UINTN)(mEPark + 0x88),
    (UINT64)(UINTN)mECtx,
    (UINT64)(UINTN)M3EarlyEntry
    );
  EAcpiLine ("dispatch");
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, M3EarlyOnReadyToBoot, NULL, &gEfiEventReadyToBootGuid, &mEReadyToBoot);
  SbLog ("=M3E ReadyToBoot event %r", Status);
}

#endif
