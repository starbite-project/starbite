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
#include <Protocol/GraphicsOutput.h>

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

//
// v2: the A55's guard while Windows' kernel runs on the M3 (position independent, copied to a reserved page at
// +0x800; x0 = that page, which holds a 4-entry level-1 table of 1 GiB identity blocks: 0..1 GiB Device-nGnRnE
// (PMU), 2..4 GiB Normal write-back (this page, the pmsg ring)). It never touches Windows' memory or devices:
//   MMU off, own MAIR/TCR/TTBR0, MMU and caches on (coherent reads of the ring whichever way Windows maps it);
//   baseline = the pmsg ring header's start/size words; then poll (clean+invalidate, read) until either changes
//   (the first line Windows' UFS driver logs as it loads, before any disk write) or 90 s pass; then ~2 ms for the
//   driver's first line to land, the whole ring cleaned to PoC (DC CIVAC is broadcast, so the M3's dirty lines too),
//   and the FADT reset register (PMU SWRESET 0x14060400 = 1).
//
extern UINT8  M3EarlyGuard[];
extern UINT8  M3EarlyGuardEnd[];

__asm__ (
  "  .text\n"
  "  .p2align 3\n"
  "  .globl M3EarlyGuard\n"
  "M3EarlyGuard:\n"
  "  adr   x19, .\n"                     // its own page (the code sits at +0x800; also entered from the M3 park loop)
  "  and   x19, x19, #0xFFFFFFFFFFFFF000\n"
  "  mrs   x1, sctlr_el1\n"
  "  bic   x1, x1, #1\n"
  "  bic   x1, x1, #4\n"
  "  bic   x1, x1, #0x1000\n"
  "  dsb   sy\n"
  "  msr   sctlr_el1, x1\n"
  "  isb\n"
  "  ic    iallu\n"
  "  tlbi  vmalle1\n"
  "  dsb   sy\n"
  "  isb\n"
  "  mov   x1, #0xFF00\n"
  "  msr   mair_el1, x1\n"
  "  movz  x1, #0x3519\n"               // v4 TCR: T0SZ 25 (512 GiB, level 1 start), WB, inner shareable, 4K,
  "  movk  x1, #0x80, lsl #16\n"         //    EPD1, IPS 40 bits
  "  movk  x1, #0x2, lsl #32\n"
  "  msr   tcr_el1, x1\n"
  "  ldr   x1, [x19, #0xA0]\n"           // v4: the 512-entry table page
  "  msr   ttbr0_el1, x1\n"
  "  isb\n"
  "  tlbi  vmalle1\n"
  "  dsb   sy\n"
  "  isb\n"
  "  movz  x1, #0x183D\n"
  "  movk  x1, #0x30C5, lsl #16\n"
  "  msr   sctlr_el1, x1\n"
  "  isb\n"
  "  movz  x20, #0x4000\n"              // pmsg ring 0xFED14000
  "  movk  x20, #0xFED1, lsl #16\n"
  "  dc    civac, x20\n"
  "  dsb   sy\n"
  "  ldr   w21, [x20, #4]\n"
  "  ldr   w22, [x20, #8]\n"
  "  mrs   x23, cntvct_el0\n"            // t0
  "  movz  x24, #0x5D40\n"              // 0.5 s = 13,000,000 ticks
  "  movk  x24, #0xC6, lsl #16\n"
  "  mov   x25, x23\n"                   // last beat
  "  mov   x26, #0\n"                    // beat
  "  mov   x27, #0\n"                    // beat GICD_CTLR first seen enabled (0 = never)
  "  mov   x28, #0\n"                    // framebuffer changes
  "  mov   x15, #0\n"                    // beat of the first change
  "  mov   x16, #0\n"                    // last hash
  "  mov   x29, #0\n"                    // v3: KUSER_SHARED_DATA's physical page once found
  "1:\n"
  "  dc    civac, x20\n"
  "  dsb   sy\n"
  "  ldr   w1, [x20, #4]\n"
  "  cmp   w1, w21\n"
  "  b.ne  2f\n"
  "  ldr   w1, [x20, #8]\n"
  "  cmp   w1, w22\n"
  "  b.ne  2f\n"
  "  mrs   x1, cntvct_el0\n"
  "  sub   x2, x1, x25\n"
  "  cmp   x2, x24\n"
  "  b.lo  1b\n"
  "  mov   x25, x1\n"
  "  add   x26, x26, #1\n"
  // framebuffer hash (sampled band, see the C side's literals at +0x50..+0x70)
  "  mov   x14, #0\n"
  "  ldr   x9, [x19, #0x50]\n"
  "  cbz   x9, 13f\n"
  "  ldr   x10, [x19, #0x58]\n"
  "  ldr   x11, [x19, #0x60]\n"
  "  ldr   x13, [x19, #0x70]\n"
  "10:\n"
  "  mov   x7, x9\n"
  "  ldr   x12, [x19, #0x68]\n"
  "11:\n"
  "  dc    civac, x7\n"
  "  dsb   ish\n"
  "  ldr   w1, [x7]\n"
  "  eor   x14, x1, x14, ror #7\n"
  "  add   x7, x7, x13\n"
  "  subs  x12, x12, #1\n"
  "  b.ne  11b\n"
  "  add   x9, x9, x10\n"
  "  subs  x11, x11, #1\n"
  "  b.ne  10b\n"
  "  cmp   x26, #1\n"
  "  b.eq  12f\n"
  "  cmp   x14, x16\n"
  "  b.eq  12f\n"
  "  add   x28, x28, #1\n"
  "  cbnz  x15, 12f\n"
  "  mov   x15, x26\n"
  "12:\n"
  "  mov   x16, x14\n"
  "13:\n"
  // GIC distributor as Windows' HAL leaves it
  "  movz  x3, #0x1000\n"
  "  movk  x3, #0x1010, lsl #16\n"
  "  ldr   w10, [x3]\n"
  "  and   x10, x10, #1\n"
  "  cbz   x10, 14f\n"
  "  cbnz  x27, 14f\n"
  "  mov   x27, x26\n"
  "14:\n"
  "  mov   x11, #0\n"
  "  mov   x6, #1\n"
  "  add   x8, x3, #0x100\n"
  "15:\n"
  "  ldr   w7, [x8, x6, lsl #2]\n"
  "  cbz   w7, 16f\n"
  "  add   x11, x11, #1\n"
  "16:\n"
  "  add   x6, x6, #1\n"
  "  cmp   x6, #16\n"
  "  b.lo  15b\n"
  // the line "=M3K b0000 e00 gd0 @00 fbn00 @00 h00000000" in zone 0 (+0x40) and pmsg (+0x48)
  "  ldr   x9, [x19, #0x40]\n"
  "  bl    20f\n"
  "  ldr   x9, [x19, #0x48]\n"
  "  bl    20f\n"
  // v3: KUSER_SHARED_DATA (NtMajorVersion 10 at +0x26C, ImageNumberLow/High 0xAA64 at +0x2C) in winload's
  // EfiLoaderData ranges (+0x100: base, pages; +0x78 count); then InterruptTime (+0x8) and TickCount (+0x320)
  "  cbnz  x29, 41f\n"
  "  ldr   x3, [x19, #0x78]\n"
  "  add   x4, x19, #0x100\n"
  "  mov   x18, #256\n"
  "40:\n"
  "  cbz   x3, 42f\n"
  "  ldp   x5, x6, [x4], #16\n"
  "44:\n"
  "  cbz   x6, 43f\n"
  "  ldr   w7, [x5, #0x26C]\n"
  "  cmp   w7, #10\n"
  "  b.ne  45f\n"
  "  ldr   w7, [x5, #0x2C]\n"
  "  movz  w8, #0xAA64\n"
  "  movk  w8, #0xAA64, lsl #16\n"
  "  cmp   w7, w8\n"
  "  b.ne  45f\n"
  "  mov   x29, x5\n"
  "  b     41f\n"
  "45:\n"
  "  add   x5, x5, #0x1000\n"
  "  sub   x6, x6, #1\n"
  "  subs  x18, x18, #1\n"              // v4: the UFS trigger every 256 pages scanned
  "  b.ne  44b\n"
  "  mov   x18, #256\n"
  "  dc    civac, x20\n"
  "  dsb   sy\n"
  "  ldr   w1, [x20, #4]\n"
  "  cmp   w1, w21\n"
  "  b.ne  2f\n"
  "  ldr   w1, [x20, #8]\n"
  "  cmp   w1, w22\n"
  "  b.ne  2f\n"
  "  b     44b\n"
  "43:\n"
  "  sub   x3, x3, #1\n"
  "  b     40b\n"
  "41:\n"
  "  ldr   w7, [x29, #0x8]\n"
  "  str   x7, [x19, #0x80]\n"
  "  ldr   w7, [x29, #0x320]\n"
  "  str   x7, [x19, #0x88]\n"
  "42:\n"
  "  ldr   x9, [x19, #0x90]\n"
  "  bl    50f\n"
  "  ldr   x9, [x19, #0x98]\n"
  "  bl    50f\n"
  "  ldr   x9, [x19, #0xB0]\n"            // v5: ntoskrnl globals line, zone 0 and pmsg
  "  bl    60f\n"
  "  ldr   x9, [x19, #0xB8]\n"
  "  bl    60f\n"
  "  mrs   x1, cntvct_el0\n"            // 120 s fallback (the loader's own watchdog is 82 s)
  "  sub   x1, x1, x23\n"
  "  movz  x2, #0xE400\n"
  "  movk  x2, #0xB9F8, lsl #16\n"
  "  cmp   x1, x2\n"
  "  b.lo  1b\n"
  "2:\n"                                // Windows' UFS driver logged (or 120 s): 2 ms, flush both rings, reset
  "  mov   w5, #0x54\n"                  // v6: mark the =M3K line: 'U' = the UFS driver's line, 'T' = timeout
  "  dc    civac, x20\n"
  "  dsb   sy\n"
  "  ldr   w1, [x20, #4]\n"
  "  ldr   w2, [x20, #8]\n"
  "  cmp   w1, w21\n"
  "  ccmp  w2, w22, #0, eq\n"
  "  mov   w6, #0x55\n"
  "  csel  w5, w5, w6, eq\n"
  "  ldr   x9, [x19, #0x40]\n"
  "  cbz   x9, 6f\n"
  "  strb  w5, [x9, #5]\n"
  "6:\n"
  "  ldr   x9, [x19, #0x48]\n"
  "  cbz   x9, 7f\n"
  "  strb  w5, [x9, #5]\n"
  "7:\n"
  "  mrs   x23, cntvct_el0\n"
  "3:\n"
  "  mrs   x1, cntvct_el0\n"
  "  sub   x1, x1, x23\n"
  "  mov   x4, #52000\n"
  "  cmp   x1, x4\n"
  "  b.lo  3b\n"
  "  movz  x2, #0x0000\n"
  "  movk  x2, #0xFED1, lsl #16\n"
  "  add   x3, x2, #0x8000\n"
  "4:\n"
  "  dc    civac, x2\n"
  "  add   x2, x2, #64\n"
  "  cmp   x2, x3\n"
  "  b.lo  4b\n"
  "  dsb   sy\n"
  // v6: not PMU SWRESET (a55ctl1: that path came back with both rings empty). The loader's own watchdog (WDT1
  // 0x10060000, shim/starlte/wdt.c wdt_arm) reloaded with a tiny count instead: the same hardware reset as every
  // m3kern run's 82 s watchdog, which lands in the loader menu with zone 0 intact.
  "  movz  x1, #0x0000\n"
  "  movk  x1, #0x1006, lsl #16\n"
  "  str   wzr, [x1]\n"
  "  dsb   sy\n"
  "  mov   w2, #0x10\n"
  "  str   w2, [x1, #4]\n"
  "  str   w2, [x1, #8]\n"
  "  str   wzr, [x1, #0xC]\n"
  "  dsb   sy\n"
  "  mov   w2, #0xFF39\n"
  "  str   w2, [x1]\n"
  "  dsb   sy\n"
  "  movz  x1, #0x0408\n"
  "  movk  x1, #0x1406, lsl #16\n"
  "  ldr   w2, [x1]\n"
  "  bic   w2, w2, #0x800000\n"
  "  str   w2, [x1]\n"
  "  ldr   w2, [x1, #4]\n"
  "  bic   w2, w2, #0x800000\n"
  "  str   w2, [x1, #4]\n"
  "  dsb   sy\n"
  "  movz  x1, #0x0000\n"
  "  movk  x1, #0x1006, lsl #16\n"
  "  mov   w2, #0x10\n"
  "  mov   w3, #0xFF39\n"
  "  mrs   x23, cntvct_el0\n"
  "  movz  x24, #0x5840\n"               // 0.2 s
  "  movk  x24, #0x4F, lsl #16\n"
  "5:\n"                                // keep forcing it: Windows' UFS driver pets this watchdog as it starts
  "  str   w2, [x1, #8]\n"
  "  str   w3, [x1]\n"
  "  dsb   sy\n"
  "  mrs   x4, cntvct_el0\n"
  "  sub   x4, x4, x23\n"
  "  cmp   x4, x24\n"
  "  b.lo  5b\n"
  "  movz  x1, #0x0400\n"                // no watchdog reset within 0.2 s: PMU SWRESET (the volume comes first)
  "  movk  x1, #0x1406, lsl #16\n"
  "  mov   w2, #1\n"
  "  str   w2, [x1]\n"
  "  dsb   sy\n"
  "8:\n"
  "  b     8b\n"
  // write the fields at line x9 (skip if 0); keeps x19..x28, x14..x16, x10, x11
  "20:\n"
  "  cbz   x9, 29f\n"
  "  mov   x17, x30\n"
  "  mov   x0, x26\n  add x1, x9, #6\n   mov x2, #4\n  bl 30f\n"
  "  mov   x0, x11\n  add x1, x9, #12\n  mov x2, #2\n  bl 30f\n"
  "  mov   x0, x10\n  add x1, x9, #17\n  mov x2, #1\n  bl 30f\n"
  "  mov   x0, x27\n  add x1, x9, #20\n  mov x2, #2\n  bl 30f\n"
  "  mov   x0, x28\n  add x1, x9, #26\n  mov x2, #2\n  bl 30f\n"
  "  mov   x0, x15\n  add x1, x9, #30\n  mov x2, #2\n  bl 30f\n"
  "  mov   x0, x14\n  add x1, x9, #34\n  mov x2, #8\n  bl 30f\n"
  "  dc    civac, x9\n"
  "  add   x0, x9, #41\n"
  "  dc    civac, x0\n"
  "  dsb   sy\n"
  "  mov   x30, x17\n"
  "29:\n"
  "  ret\n"
  // "=M3U u00000000 it00000000 tc00000000" at x9 (skip if 0)
  "50:\n"
  "  cbz   x9, 59f\n"
  "  mov   x17, x30\n"
  "  mov   x0, x29\n          add x1, x9, #6\n   mov x2, #8\n  bl 30f\n"
  "  ldr   x0, [x19, #0x80]\n add x1, x9, #17\n  mov x2, #8\n  bl 30f\n"
  "  ldr   x0, [x19, #0x88]\n add x1, x9, #28\n  mov x2, #8\n  bl 30f\n"
  "  dc    civac, x9\n"
  "  add   x0, x9, #35\n"
  "  dc    civac, x0\n"
  "  dsb   sy\n"
  "  mov   x30, x17\n"
  "59:\n"
  "  ret\n"
  // v5: "=M3N lb00000000 np00 ps00000000 bc00000000 p10000000000000000" at x9: KeLoaderBlock, KeNumberProcessors,
  // PsInitialSystemProcess, KiBugCheckData[0], [1] read through ntoskrnl's physical image (+0xC0..+0xD8)
  "60:\n"
  "  cbz   x9, 69f\n"
  "  mov   x17, x30\n"
  "  ldr   x3, [x19, #0xC0]\n"
  "  cbz   x3, 68f\n"
  "  ldr   x0, [x3]\n              add x1, x9, #7\n   mov x2, #8\n   bl 30f\n"
  "  ldr   x3, [x19, #0xC8]\n  ldrb w0, [x3]\n  add x1, x9, #18\n  mov x2, #2\n   bl 30f\n"
  "  ldr   x3, [x19, #0xD0]\n  ldr x0, [x3]\n   add x1, x9, #23\n  mov x2, #8\n   bl 30f\n"
  "  ldr   x3, [x19, #0xD8]\n  ldr x0, [x3]\n   add x1, x9, #34\n  mov x2, #8\n   bl 30f\n"
  "  ldr   x3, [x19, #0xD8]\n  ldr x0, [x3, #8]\n add x1, x9, #45\n mov x2, #16\n  bl 30f\n"
  "  dc    civac, x9\n"
  "  add   x0, x9, #60\n"
  "  dc    civac, x0\n"
  "  dsb   sy\n"
  "68:\n"
  "  mov   x30, x17\n"
  "69:\n"
  "  ret\n"
  "30:\n"                               // x0 value, x1 first digit, x2 digits; clobbers x0-x2, x6-x8
  "  add   x1, x1, x2\n"
  "31:\n"
  "  sub   x1, x1, #1\n"
  "  and   x6, x0, #0xF\n"
  "  add   x7, x6, #0x30\n"
  "  add   x8, x6, #0x37\n"
  "  cmp   x6, #10\n"
  "  csel  x7, x7, x8, lo\n"
  "  strb  w7, [x1]\n"
  "  lsr   x0, x0, #4\n"
  "  subs  x2, x2, #1\n"
  "  b.ne  31b\n"
  "  ret\n"
  "  .globl M3EarlyGuardEnd\n"
  "M3EarlyGuardEnd:\n"
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
STATIC EFI_PHYSICAL_ADDRESS    mEGuard;            // v2: the A55's guard page (L1 table at +0, code at +0x800)
STATIC EFI_EXIT_BOOT_SERVICES  mEOrigEbs;
STATIC EFI_EVENT               mEReadyToBoot;
STATIC BOOLEAN                 mEMoved;
STATIC BOOLEAN                 mEFired;
STATIC UINT32                  mEEn0, mEPri[8], mECtlr, mEPmr, mEBpr;
STATIC UINT64                  mEPctl, mEPcval, mEVctl, mEVcval;
STATIC UINTN                   mEWatch[2][4];      // [ring][field] first digit: beat prog pmsg; [3] the line's start
STATIC CONST CHAR8             mEWatchTag[] = "=M3E watch beat ";
STATIC CONST CHAR8             mEKTag[]     = "=M3K b0000 e00 gd0 @00 fbn00 @00 h00000000";
STATIC CONST CHAR8             mEUTag[]     = "=M3U u00000000 it00000000 tc00000000";
STATIC CONST CHAR8             mENTag[]     = "=M3N lb00000000 np00 ps00000000 bc00000000 p10000000000000000";
STATIC UINT8                   mEMap[0x8000];      // v3: the memory map at ExitBootServices (static: no allocation)

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
 #if STARBITE_M3_EARLY >= 2
    __asm__ __volatile__ ("dc civac, %0\n dsb sy" : : "r" ((UINTN)&Ctx->Pad[1]) : "memory");
    if (Ctx->Pad[1] == 7) {
      //
      // The M3 is about to return into winload with the committed tables: from here on Windows may reclaim
      // boot-services memory (this code, UEFI's page tables). Into the guard in reserved memory, own tables.
      //
      ((volatile UINT64 *)(UINTN)mEGuard)[0x40 / 8] = ERingFind (SB_EVLOG_BASE, SB_EVLOG_CAP, mEKTag);
      ((volatile UINT64 *)(UINTN)mEGuard)[0x48 / 8] = ERingFind (SB_PMSG_BASE, SB_PMSG_CAP, mEKTag);
      ((volatile UINT64 *)(UINTN)mEGuard)[0x90 / 8] = ERingFind (SB_EVLOG_BASE, SB_EVLOG_CAP, mEUTag);
      ((volatile UINT64 *)(UINTN)mEGuard)[0x98 / 8] = ERingFind (SB_PMSG_BASE, SB_PMSG_CAP, mEUTag);
      ((volatile UINT64 *)(UINTN)mEGuard)[0xB0 / 8] = ERingFind (SB_EVLOG_BASE, SB_EVLOG_CAP, mENTag);
      ((volatile UINT64 *)(UINTN)mEGuard)[0xB8 / 8] = ERingFind (SB_PMSG_BASE, SB_PMSG_CAP, mENTag);
      EClean ((UINTN)mEGuard, (UINTN)mEGuard + 0x800);
      EClean (SB_EVLOG_BASE, SB_EVLOG_BASE + 12 + SB_EVLOG_CAP);
      EClean (SB_PMSG_BASE, SB_PMSG_BASE + 12 + SB_PMSG_CAP);
      __asm__ __volatile__ ("mov x0, %0\n br %1" : : "r" ((UINT64)mEGuard), "r" ((UINT64)mEGuard + 0x800) : "x0", "memory");
    }

 #endif
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

//
// v2, on the M3 after the real ExitBootServices (woa's rewrite of FADT/MADT is long done by then): the MADT gets
// M3 core 1 (UID 5) as its first and only enabled GICC (boot CPU: no parking mailbox), every other GICC disabled;
// every DSDT _MAT GICC gets the same Enabled bit as its MADT twin. Checksums fixed. ARM_BOOT_ARCH stays 0
// (parking protocol, one CPU). Logged.
//
//
// v3: a band of the GOP framebuffer for the guard's hash (rows 5/8..7/8 of the height every 8th row, every 32nd
// pixel): +0x50 first address, +0x58 row step, +0x60 rows, +0x68 columns, +0x70 column step. 0 = no framebuffer.
//
STATIC
VOID
EFbLiterals (
  VOID
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL  *Gop;
  volatile UINT64               *L = (volatile UINT64 *)(UINTN)mEGuard;
  UINT64                        Base;
  UINT64                        Ppsl;
  UINT64                        H;
  UINT64                        W;

  L[0x50 / 8] = 0;
  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) || (Gop->Mode == NULL) || (Gop->Mode->Info == NULL)) {
    SbLog ("=M3E fb: no GOP");
    return;
  }

  Base = Gop->Mode->FrameBufferBase;
  Ppsl = Gop->Mode->Info->PixelsPerScanLine;
  H    = Gop->Mode->Info->VerticalResolution;
  W    = Gop->Mode->Info->HorizontalResolution;
  if ((Base < 0x80000000ull) || (Base + Ppsl * H * 4 > 0x100000000ull) || (H < 64) || (W < 64)) {
    SbLog ("=M3E fb: %lx %ldx%ld outside the guard's map, no hash", Base, W, H);
    return;
  }

  L[0x58 / 8] = Ppsl * 4 * 32;              // v5: the whole screen, every 32nd row and 32nd pixel
  L[0x60 / 8] = H / 32;
  L[0x68 / 8] = W / 32;
  L[0x70 / 8] = 32 * 4;
  L[0x50 / 8] = Base;
  EClean ((UINTN)mEGuard, (UINTN)mEGuard + 0x80);
  SbLog ("=M3E fb %lx %ldx%ld ppsl %ld", Base, W, H, Ppsl);
}

//
// v5: ntoskrnl's physical image (winload loads it contiguously; woa logs the same base, =NTID b=BA000000): the guard
// reads four exported globals through it. Only for the analysed build (SizeOfImage 0x103D000, export name
// ntoskrnl.exe; RVAs from its export table): KeLoaderBlock D1B4B8, KeNumberProcessors D1AB86, PsInitialSystemProcess
// D1AA20, KiBugCheckData CFE0A0. Candidates: 0xBA000000 and the start of every EfiLoaderCode range.
//
STATIC
BOOLEAN
EIsNtos (
  IN UINT64  B
  )
{
  UINT8   *P = (UINT8 *)(UINTN)B;
  UINT32  Pe;
  UINT32  Exp;
  UINT32  Name;

  if ((B == 0) || (P[0] != 'M') || (P[1] != 'Z')) {
    return FALSE;
  }

  CopyMem (&Pe, P + 0x3C, 4);
  if ((Pe > 0x400) || (CompareMem (P + Pe, "PE\0\0", 4) != 0)) {
    return FALSE;
  }

  CopyMem (&Exp, P + Pe + 24 + 56, 4);       // SizeOfImage (PE32+ optional header +56)
  if (Exp != 0x103D000u) {
    return FALSE;
  }

  CopyMem (&Exp, P + Pe + 24 + 112, 4);      // export directory RVA (PE32+)
  if ((Exp == 0) || (Exp >= 0x103D000u)) {
    return FALSE;
  }

  CopyMem (&Name, P + Exp + 12, 4);
  return (Name < 0x103D000u) && (AsciiStrnCmp ((CHAR8 *)P + Name, "ntoskrnl.exe", 12) == 0);
}

STATIC
VOID
ENtosFind (
  VOID
  )
{
  volatile UINT64        *L = (volatile UINT64 *)(UINTN)mEGuard;
  EFI_MEMORY_DESCRIPTOR  *D;
  UINT64                 B;
  UINTN                  i;

  B = 0;
  if (EIsNtos (0xBA000000ull)) {
    B = 0xBA000000ull;
  }

  for (i = 0; (B == 0) && (i < 112); i++) {
    if ((L[(0x100 / 8) + 2 * i + 1] != 0) && EIsNtos (L[(0x100 / 8) + 2 * i])) {
      B = L[(0x100 / 8) + 2 * i];
    }
  }

  (VOID)D;
  L[0xA8 / 8] = B;
  L[0xC0 / 8] = B ? B + 0xD1B4B8 : 0;
  L[0xC8 / 8] = B ? B + 0xD1AB86 : 0;
  L[0xD0 / 8] = B ? B + 0xD1AA20 : 0;
  L[0xD8 / 8] = B ? B + 0xCFE0A0 : 0;
  EClean ((UINTN)mEGuard, (UINTN)mEGuard + 0x800);
  SbLog ("=M3E ntos %lx lb %lx np %x bc %lx", B, B ? *(UINT64 *)(UINTN)(B + 0xD1B4B8) : 0, B ? *(UINT8 *)(UINTN)(B + 0xD1AB86) : 0, B ? *(UINT64 *)(UINTN)(B + 0xCFE0A0) : 0);
}

//
// v4: RAM the kernel may use, from the memory map at ExitBootServices (static buffer: no allocation, winload's map
// key stays valid), for the guard's KUSER_SHARED_DATA scan: first EfiLoaderData and OS-defined types, then loader
// code, boot services code/data and conventional memory; adjacent ranges merged; 2..64 GiB (the guard's map); never
// reserved, runtime, ACPI, MMIO or unusable memory. Up to 112 ranges at +0x100. One short line per memory type.
//
STATIC
BOOLEAN
EScanType (
  IN UINT32  Type,
  IN UINTN   Pass
  )
{
  if (Pass == 0) {
    return (Type == EfiLoaderData) || (Type >= 0x70000000u);
  }

  return (Type == EfiLoaderCode) || (Type == EfiBootServicesCode) || (Type == EfiBootServicesData) || (Type == EfiConventionalMemory);
}

STATIC
VOID
ELoaderRanges (
  VOID
  )
{
  volatile UINT64        *L = (volatile UINT64 *)(UINTN)mEGuard;
  EFI_MEMORY_DESCRIPTOR  *D;
  UINTN                  Size;
  UINTN                  Key;
  UINTN                  DSize;
  UINT32                 DVer;
  UINTN                  n;
  UINTN                  Pass;
  UINTN                  Dropped;
  UINTN                  t;
  UINT64                 Pages[17], Lo[17], Hi[17], Cnt[17];
  UINT64                 End;
  EFI_STATUS             Status;

  Size   = sizeof (mEMap);
  Status = gBS->GetMemoryMap (&Size, (EFI_MEMORY_DESCRIPTOR *)mEMap, &Key, &DSize, &DVer);
  L[0x78 / 8] = 0;
  if (EFI_ERROR (Status) || (DSize == 0)) {
    SbLog ("=M3E map: %r size %ld", Status, Size);
    return;
  }

  ZeroMem (Pages, sizeof (Pages));
  ZeroMem (Cnt, sizeof (Cnt));
  SetMem64 (Lo, sizeof (Lo), MAX_UINT64);
  ZeroMem (Hi, sizeof (Hi));
  n       = 0;
  Dropped = 0;
  for (Pass = 0; Pass < 2; Pass++) {
    for (D = (EFI_MEMORY_DESCRIPTOR *)mEMap; (UINT8 *)D < mEMap + Size; D = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)D + DSize)) {
      End = D->PhysicalStart + EFI_PAGES_TO_SIZE (D->NumberOfPages);
      if (Pass == 0) {
        t         = (D->Type < 16) ? D->Type : 16;
        Pages[t] += D->NumberOfPages;
        Cnt[t]++;
        Lo[t] = MIN (Lo[t], D->PhysicalStart);
        Hi[t] = MAX (Hi[t], End);
      }

      if (!EScanType (D->Type, Pass) ||
          !(((D->PhysicalStart >= 0x80000000ull) && (End <= 0x100000000ull)) ||
            ((D->PhysicalStart >= 0x880000000ull) && (End <= 0x900000000ull))))
      {
        continue;
      }

      if ((n > 0) && (L[(0x100 / 8) + 2 * (n - 1)] + EFI_PAGES_TO_SIZE (L[(0x100 / 8) + 2 * (n - 1) + 1]) == D->PhysicalStart)) {
        L[(0x100 / 8) + 2 * (n - 1) + 1] += D->NumberOfPages;
        continue;
      }

      if (n >= 112) {
        Dropped++;
        continue;
      }

      L[(0x100 / 8) + 2 * n]     = D->PhysicalStart;
      L[(0x100 / 8) + 2 * n + 1] = D->NumberOfPages;
      n++;
    }
  }

  L[0x78 / 8] = n;
  EClean ((UINTN)mEGuard, (UINTN)mEGuard + 0x800);
  SbLog ("=M3E map: %ld entries, %d scan ranges, %d dropped", Size / DSize, n, Dropped);
  for (t = 0; t < 17; t++) {
    if (Cnt[t] != 0) {
      SbLog ("=M3E mt %x n%ld p%lx %lx-%lx", (UINT32)t, Cnt[t], Pages[t], Lo[t], Hi[t]);
    }
  }
}

STATIC
VOID
EFixSum (
  IN UINT8  *T
  )
{
  UINT32  Len;
  UINT32  i;
  UINT8   Sum;

  CopyMem (&Len, T + 4, 4);
  T[9] = 0;
  Sum  = 0;
  for (i = 0; i < Len; i++) {
    Sum = (UINT8)(Sum + T[i]);
  }

  T[9] = (UINT8)(0 - Sum);
}

STATIC
VOID
ECommitTables (
  VOID
  )
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp;
  EFI_ACPI_DESCRIPTION_HEADER                   *X;
  UINT8                                         *T;
  UINT8                                         *Madt;
  UINT8                                         *Dsdt;
  UINT8                                         Tmp[80];
  UINT64                                        E;
  UINT32                                        Len;
  UINT32                                        Off;
  UINT32                                        First;
  UINT32                                        Boot;
  UINT32                                        Uid;
  UINT32                                        Fl;
  UINTN                                         i;
  UINTN                                         Mats;

  Rsdp = NULL;
  for (i = 0; i < gST->NumberOfTableEntries; i++) {
    if (CompareGuid (&gST->ConfigurationTable[i].VendorGuid, &mEAcpi20Guid)) {
      Rsdp = gST->ConfigurationTable[i].VendorTable;
    }
  }

  if ((Rsdp == NULL) || (Rsdp->XsdtAddress == 0)) {
    SbLog ("=M3E commit: no XSDT");
    return;
  }

  X    = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
  Madt = NULL;
  Dsdt = NULL;
  for (i = 0; i < (X->Length - sizeof (*X)) / 8; i++) {
    CopyMem (&E, (UINT8 *)(X + 1) + 8 * i, 8);
    T = (UINT8 *)(UINTN)E;
    if (T == NULL) {
      continue;
    }

    if (CompareMem (T, "APIC", 4) == 0) {
      Madt = T;
    } else if (CompareMem (T, "FACP", 4) == 0) {
      CopyMem (&E, T + 140, 8);
      if (E == 0) {
        CopyMem (&Off, T + 40, 4);
        E = Off;
      }

      Dsdt = (UINT8 *)(UINTN)E;
    }
  }

  if (Madt == NULL) {
    SbLog ("=M3E commit: no MADT");
    return;
  }

  CopyMem (&Len, Madt + 4, 4);
  First = 0;
  Boot  = 0;
  for (Off = 44; Off + 2 <= Len && Madt[Off + 1] != 0; Off += Madt[Off + 1]) {
    if ((Madt[Off] != 0x0B) || (Madt[Off + 1] < 80)) {
      continue;
    }

    if (First == 0) {
      First = Off;
    }

    CopyMem (&Uid, Madt + Off + 8, 4);
    CopyMem (&Fl, Madt + Off + 12, 4);
    Fl = (Uid == 5) ? (Fl | 1) : (Fl & ~1u);
    CopyMem (Madt + Off + 12, &Fl, 4);
    if (Uid == 5) {
      Boot = Off;
      ZeroMem (Madt + Off + 16, 4);     // parking protocol version 0
      ZeroMem (Madt + Off + 24, 8);     // no mailbox (boot CPU)
    }
  }

  if ((First == 0) || (Boot == 0)) {
    SbLog ("=M3E commit: GICC entries not found (first %x uid5 %x), MADT unchanged", First, Boot);
    return;
  }

  if (Boot != First) {
    CopyMem (Tmp, Madt + First, 80);
    CopyMem (Madt + First, Madt + Boot, 80);
    CopyMem (Madt + Boot, Tmp, 80);
  }

  EFixSum (Madt);
  Mats = 0;
  if (Dsdt != NULL) {
    CopyMem (&Len, Dsdt + 4, 4);
    for (Off = 36; Off + 4 < Len; Off++) {
      if (CompareMem (Dsdt + Off, "_MAT", 4) != 0) {
        continue;
      }

      for (i = Off + 4; (i < Off + 20) && (i + 80 <= Len); i++) {
        if ((Dsdt[i] == 0x0B) && (Dsdt[i + 1] == 0x50)) {
          CopyMem (&Uid, Dsdt + i + 8, 4);
          CopyMem (&Fl, Dsdt + i + 12, 4);
          Fl = (Uid == 5) ? (Fl | 1) : (Fl & ~1u);
          CopyMem (Dsdt + i + 12, &Fl, 4);
          Mats++;
          break;
        }
      }
    }

    EFixSum (Dsdt);
  }

  SbLog ("=M3E commit: MADT GICC UID 5 (M3 1, mpidr 101, if 5) first and the only one enabled; %d DSDT _MAT copies synced", Mats);
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
 #if STARBITE_M3_EARLY >= 2
    ELoaderRanges ();
    ENtosFind ();
 #endif
  }

  Status = mEOrigEbs (ImageHandle, MapKey);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  mSbAfterEbs = TRUE;
  EProgress (6);
  SbLog ("=M3E ExitBootServices returned %r on mpidr %lx (woa's hook included)", Status, EMpidr ());
  EAcpiLine ("ebs-post");
 #if STARBITE_M3_EARLY == 3
  {
    volatile UINT64  *L = (volatile UINT64 *)(UINTN)mEGuard;

    L[0x40 / 8] = ERingFind (SB_EVLOG_BASE, SB_EVLOG_CAP, mEKTag);
    L[0x48 / 8] = ERingFind (SB_PMSG_BASE, SB_PMSG_CAP, mEKTag);
    L[0x90 / 8] = ERingFind (SB_EVLOG_BASE, SB_EVLOG_CAP, mEUTag);
    L[0x98 / 8] = ERingFind (SB_PMSG_BASE, SB_PMSG_CAP, mEUTag);
    L[0xB0 / 8] = ERingFind (SB_EVLOG_BASE, SB_EVLOG_CAP, mENTag);
    L[0xB8 / 8] = ERingFind (SB_PMSG_BASE, SB_PMSG_CAP, mENTag);
    SbLog ("=M3E ctl: guard released on M3 core 1; Windows continues on A55 0 (MADT as woa made it)");
    EClean ((UINTN)mEGuard, (UINTN)mEGuard + 0x800);
    EClean (SB_EVLOG_BASE, SB_EVLOG_BASE + 12 + SB_EVLOG_CAP);
    EClean (SB_PMSG_BASE, SB_PMSG_BASE + 12 + SB_PMSG_CAP);
    *(volatile UINT32 *)(UINTN)(mEPark + 0x40) = 1;
    EClean ((UINTN)mEPark + 0x40, (UINTN)mEPark + 0x44);
    __asm__ __volatile__ ("sev" ::: "memory");
    return Status;
  }
 #endif
 #if STARBITE_M3_EARLY >= 2
  if (mEMoved) {
    ECommitTables ();
    EAcpiLine ("committed");
    SbLog ("=M3E v2: returning into winload on the M3; kernel starts. A55 0 resets at the UFS driver's 1st pmsg line");
    EProgress (7);
    return Status;
  }

 #endif
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
 #if STARBITE_M3_EARLY >= 2
  EFbLiterals ();
  SbLog ("%a", mEKTag);
  SbLog ("%a", mEUTag);
  SbLog ("%a", mENTag);
 #endif
 #if STARBITE_M3_EARLY == 3
  SbLog ("=M3E ctl: no hand-off; Windows boots on the A55s, the guard runs on M3 core 1 from EBS");
  return;
 #endif
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

  SbLog (
    "=M3E v%d armed: %a",
    STARBITE_M3_EARLY,
    (STARBITE_M3_EARLY >= 2) ? "boot CPU -> M3 core 1 at ReadyToBoot; KERNEL ATTEMPT on the M3, A55 0 resets at the UFS driver's first pmsg line or 90 s"
                             : "log only; boot CPU -> M3 core 1 at ReadyToBoot (before winload), reset at ExitBootServices"
    );
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
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, 5, &Page))) {
    SbLog ("=M3E: no pages, not armed");
    return;
  }

  ZeroMem ((VOID *)(UINTN)Page, 5 * EFI_PAGE_SIZE);
  mECtx        = (M3E_CTX *)(UINTN)Page;
  mEPark       = Page + EFI_PAGE_SIZE;
  mEWatchStack = Page + 2 * EFI_PAGE_SIZE;
  ((UINT32 *)(UINTN)mEWatchStack)[0] = 0xD503207Fu;           // WFI
  ((UINT32 *)(UINTN)mEWatchStack)[1] = 0x17FFFFFFu;           // B .-4
  CopyMem ((VOID *)(UINTN)(mEPark + 0x800), M3EarlyPark, (UINTN)(M3EarlyParkEnd - M3EarlyPark));
  Lit    = (UINT64 *)(UINTN)(mEPark + 0x800 + (UINTN)(M3EarlyParkLit - M3EarlyPark));
  Lit[0] = mEPark;
  Lit[1] = (UINT64)(UINTN)M3EarlyEntry;
 #if STARBITE_M3_EARLY == 3
  Lit[1] = (UINT64)(Page + 3 * EFI_PAGE_SIZE + 0x800);      // control: the parked M3 runs the guard instead
 #endif
  mEGuard = Page + 3 * EFI_PAGE_SIZE;
  ((UINT64 *)(UINTN)mEGuard)[0] = 0x0000000000000401ull;     // 0..1 GiB Device-nGnRnE (PMU), AF
  ((UINT64 *)(UINTN)mEGuard)[1] = 0x0000000040000401ull;
  ((UINT64 *)(UINTN)mEGuard)[2] = 0x0000000080000705ull;     // 2..3 GiB Normal WB, inner shareable, AF
  ((UINT64 *)(UINTN)mEGuard)[3] = 0x00000000C0000705ull;     // 3..4 GiB (pmsg ring 0xFED14000)
  {
    UINT64  *Tbl = (UINT64 *)(UINTN)(Page + 4 * EFI_PAGE_SIZE);    // v4: 512 x 1 GiB, level 1 (T0SZ 25)
    UINTN   k;

    for (k = 0; k < 2; k++) {
      Tbl[k] = ((UINT64)k << 30) | 0x401ull;                          // 0..2 GiB Device (PMU, GIC)
    }

    Tbl[2]  = 0x0000000080000705ull;                                 // 2..4 GiB Normal WB: low DRAM, the rings
    Tbl[3]  = 0x00000000C0000705ull;
    Tbl[34] = 0x0000000880000705ull;                                 // 34..36 GiB: the high DRAM bank only
    Tbl[35] = 0x00000008C0000705ull;

    ((UINT64 *)(UINTN)mEGuard)[0xA0 / 8] = (UINT64)(UINTN)Tbl;
  }
  CopyMem ((VOID *)(UINTN)(mEGuard + 0x800), M3EarlyGuard, (UINTN)(M3EarlyGuardEnd - M3EarlyGuard));
  EClean ((UINTN)Page, (UINTN)Page + 5 * EFI_PAGE_SIZE);
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
