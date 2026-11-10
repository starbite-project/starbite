/** @file
  M3Diag (STARBITE_M3_DIAG=1): the clean-slate big-core track's first image, docs/WIN11-M3-CLEAN.md section 4.
  LOG ONLY. Windows' kernel never starts on this image: after ExitBootServices the A55 logs and resets.

  1. Dispatch (before the woa PSCI EL2 shim, PSCI_VERSION still FFFFFFFF): ONE CPU_ON of M3 core 2 (target 0x102,
     the six PMU defaults first, as every M3 start since B1) into a stub with MMU off and its own EL1 vectors. The
     stub executes, one at a time, the system instructions Windows' kernel uses and Linux on the M3 never does
     (ACTLR_EL1, PMU registers, set/way cache maintenance, debug/OS-lock registers, SMCCC queries, ...) plus the
     M3's ID registers and its banked GIC view:
       - each test writes "started i" first, then its value, the ESR of any EL1 exception it took (the stub's
         vectors record it and skip the instruction), and its duration in CNTVCT ticks;
       - an instruction that traps to an EL2 nobody serves never comes back: the crumb stops at its index. That is
         the failure mode that matches every silent hang of the old hand-off (catch 00, no exception at EL1);
       - a duration far above its siblings (tens of ticks at 26 MHz) means the instruction trapped to an EL2 that
         emulated it.
     v1 (run 2026-11-10 19:58): every test returned natively; HVC is UNDEFINED at EL1 on the M3 (HCD).
     v2 drops the HVC test and adds, each after the A55 logged the previous phase:
       phase 2: the virtual timer's PPI 27 delivered to the M3 at EL1 (priority, enable, a 100 us timer);
       phase 3: CoreSight external debug of A55 core 0 read by the M3 (MMU off): IDs, EDPRSR, EDPCSR samples
                before and after unlocking (EDLAR, OSLAR), to learn whether a hung core's PC can be sampled.
  2. ACPI snapshots at dispatch, ReadyToBoot, ExitBootServices before the inner (woa) hook, and after it: FADT
     ARM_BOOT_ARCH / flags / hash, MADT, GTDT, DSDT hashes, so the step that clears ARM_BOOT_ARCH is bracketed.
     After ExitBootServices: full hex dumps of the live FADT, MADT and GTDT, every DSDT _MAT GICC, and the A55's
     GIC CPU interface / distributor / timer state at that moment (what Windows would inherit).
  3. Then gRT->ResetSystem (warm). If it returns, the A55 waits in WFI for the loader's cluster-0 watchdog
     (about 2 minutes, as after every solo hang). Windows never runs, so its volume and registry are not touched.

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

#ifndef STARBITE_M3_DIAG
#define STARBITE_M3_DIAG 0
#endif

#if STARBITE_M3_DIAG

#include "StarbiteLog.h"

#define GICD_FALLBACK  0x10101000UL     // exynos9810.dtsi gic: GIC-400 distributor
#define GICC_FALLBACK  0x10102000UL     // CPU interface

//
// Data page (MMU off on the M3, so its stores go straight to memory):
//   +0x00 crumb (index of the test started last; 0xFE = phase 1 done, waiting for Go; 0xFF = all done)
//   +0x08 EL1 exceptions taken   +0x10 CurrentEL at entry
//   +0x40 + i * 0x20: test i: value, ESR (0 = none), ticks, ELR of the exception
//   +0xFC0 Go (the A55's only write after the CPU_ON, in a cache line of its own)
//
#define D_CRUMB  0x00
#define D_EXC    0x08
#define D_EL     0x10
#define D_REC    0x40
#define D_GO     0xFC0
#define N_TESTS  80
#define CS_A55_0 0x1E810000UL     // exynos9810.dtsi coresight@1e000000 cl0_cpu0 dbg-offset 0x810000

extern UINT8  M3DiagBlob[];
extern UINT8  M3DiagEntry[];
extern UINT8  M3DiagLit[];
extern UINT8  M3DiagEnd[];

//
// The blob: 16 vector slots (0x800 bytes, copied to a page so VBAR_EL1 is 2 KiB aligned), the entry at +0x800,
// literals at the end (data page, GICD, GICC). Registers: x19 data, x20 GICD, x27 GICC, x21 test index, x22 start
// tick; the handler uses x25/x26 only. SMCCC preserves x18..x30 across the SMCs.
//
__asm__ (
  "  .text\n"
  "  .p2align 12\n"
  "  .globl M3DiagBlob\n"
  "M3DiagBlob:\n"
  ".Ldblob:\n"
  ".irp k, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
  "  .if (\\k == 1) || (\\k == 5)\n"
  "  b     .Ldirq\n"
  "  .else\n"
  "  b     .Ldexc\n"
  "  .endif\n"
  "  .skip 0x7C\n"
  "  .endr\n"
  "  .globl M3DiagEntry\n"
  "M3DiagEntry:\n"
  "  adr   x9, .Ldlit\n"
  "  ldr   x19, [x9]\n"
  "  ldr   x20, [x9, #8]\n"
  "  ldr   x27, [x9, #16]\n"
  "  ldr   x28, [x9, #24]\n"
  "  adr   x9, .Ldblob\n"
  "  msr   vbar_el1, x9\n"
  "  isb\n"
  "  mov   x21, #0\n"
  "  mrs   x0, CurrentEL\n"
  "  str   x0, [x19, #0x10]\n"
  "  dsb   sy\n"
  ".macro DT idx\n"
  "  mov   x21, #\\idx\n"
  "  str   x21, [x19]\n"
  "  dsb   sy\n"
  "  mov   x0, #0\n"
  "  isb\n"
  "  mrs   x22, cntvct_el0\n"
  ".endm\n"
  ".macro DE idx\n"
  "  isb\n"
  "  mrs   x23, cntvct_el0\n"
  "  sub   x23, x23, x22\n"
  "  add   x9, x19, #(0x40 + \\idx * 0x20)\n"
  "  str   x0, [x9]\n"
  "  str   x23, [x9, #16]\n"
  "  dsb   sy\n"
  ".endm\n"
  ".macro RD idx, reg\n"
  "  DT \\idx\n"
  "  mrs   x0, \\reg\n"
  "  DE \\idx\n"
  ".endm\n"
  // write back the value test \src read, only if that read took no exception
  ".macro WB idx, src, reg\n"
  "  DT \\idx\n"
  "  ldr   x1, [x19, #(0x40 + \\src * 0x20 + 8)]\n"
  "  cbnz  x1, 91f\n"
  "  ldr   x1, [x19, #(0x40 + \\src * 0x20)]\n"
  "  msr   \\reg, x1\n"
  "  isb\n"
  "  mov   x0, #1\n"
  "91:\n"
  "  DE \\idx\n"
  ".endm\n"
  ".macro LDG idx, base, off\n"
  "  DT \\idx\n"
  "  ldr   w0, [\\base, #\\off]\n"
  "  DE \\idx\n"
  ".endm\n"
  "  RD 1, midr_el1\n"
  "  RD 2, mpidr_el1\n"
  "  RD 3, revidr_el1\n"
  "  RD 4, id_aa64pfr0_el1\n"
  "  RD 5, id_aa64pfr1_el1\n"
  "  RD 6, id_aa64isar0_el1\n"
  "  RD 7, id_aa64isar1_el1\n"
  "  RD 8, id_aa64mmfr0_el1\n"
  "  RD 9, id_aa64mmfr1_el1\n"
  "  RD 10, s3_0_c0_c7_2\n"             // ID_AA64MMFR2_EL1
  "  RD 11, id_aa64dfr0_el1\n"
  "  RD 12, id_aa64afr0_el1\n"
  "  RD 13, ctr_el0\n"
  "  RD 14, clidr_el1\n"
  "  RD 15, dczid_el0\n"
  "  RD 16, sctlr_el1\n"
  "  RD 17, cpacr_el1\n"
  "  RD 18, cntfrq_el0\n"
  "  RD 19, cntkctl_el1\n"
  "  RD 20, actlr_el1\n"                 // HCR_EL2.TACR
  "  WB 21, 20, actlr_el1\n"
  "  RD 22, pmcr_el0\n"                  // MDCR_EL2.TPMCR / TPM
  "  RD 23, pmceid0_el0\n"
  "  RD 24, pmuserenr_el0\n"
  "  RD 25, pmccfiltr_el0\n"
  "  RD 26, pmccntr_el0\n"
  "  RD 27, pmcntenset_el0\n"
  "  RD 28, pmselr_el0\n"
  "  RD 29, oslsr_el1\n"                 // MDCR_EL2.TDOSA
  "  RD 30, mdscr_el1\n"                 // MDCR_EL2.TDA
  "  RD 31, dbgbcr0_el1\n"
  "  DT 32\n"                            // HCR_EL2.TID2
  "  msr   csselr_el1, xzr\n"
  "  isb\n"
  "  mrs   x0, ccsidr_el1\n"
  "  DE 32\n"
  "  DT 33\n"                            // HCR_EL2.TSW: set/way by Windows' cache sweeps; L1 set 0 way 0
  "  dc    csw, x0\n"
  "  dsb   sy\n"
  "  DE 33\n"
  "  DT 34\n"
  "  dc    cisw, x0\n"
  "  dsb   sy\n"
  "  DE 34\n"
  "  DT 35\n"
  "  at    s1e1r, x19\n"
  "  isb\n"
  "  mrs   x0, par_el1\n"
  "  DE 35\n"
  "  DT 36\n"                            // HCR_EL2.TTLB
  "  tlbi  vmalle1\n"
  "  dsb   sy\n"
  "  DE 36\n"
  "  RD 37, contextidr_el1\n"            // HCR_EL2.TVM (writes)
  "  WB 38, 37, contextidr_el1\n"
  "  RD 39, mair_el1\n"
  "  WB 40, 39, mair_el1\n"
  "  RD 41, isr_el1\n"
  "  LDG 42, x20, 0x000\n"               // GICD_CTLR
  "  LDG 43, x20, 0x004\n"               // GICD_TYPER
  "  LDG 44, x20, 0x008\n"               // GICD_IIDR
  "  LDG 45, x20, 0x100\n"               // GICD_ISENABLER0 (banked)
  "  LDG 46, x20, 0x800\n"               // GICD_ITARGETSR0 (banked: this core's interface bit)
  "  LDG 47, x20, 0x418\n"               // GICD_IPRIORITYR6 (INTIDs 24..27)
  "  LDG 48, x20, 0x41C\n"               // GICD_IPRIORITYR7 (28..31)
  "  LDG 49, x27, 0x000\n"               // GICC_CTLR
  "  LDG 50, x27, 0x004\n"               // GICC_PMR
  "  LDG 51, x27, 0x018\n"               // GICC_HPPIR
  "  LDG 52, x27, 0x0FC\n"               // GICC_IIDR
  "  DT 53\n"
  "  mov   w0, #0x80000000\n"            // SMCCC_VERSION
  "  smc   #0\n"
  "  DE 53\n"
  "  DT 54\n"
  "  mov   w0, #0x84000000\n"            // PSCI_VERSION
  "  smc   #0\n"
  "  DE 54\n"
  "  DT 55\n"
  "  mov   w0, #0x80000001\n"            // SMCCC_ARCH_FEATURES(SMCCC_ARCH_WORKAROUND_1)
  "  mov   w1, #0x80008000\n"
  "  smc   #0\n"
  "  DE 55\n"
  "  mov   x9, #0xF4\n"                  // event stream (B2b: wakes WFE ~400/s on an M3)
  "  msr   cntkctl_el1, x9\n"
  "  isb\n"
  "  mov   x21, #0xFE\n"
  "  str   x21, [x19]\n"
  "  dsb   sy\n"
  "1:\n"
  "  wfe\n"
  "  ldr   x9, [x19, #0xFC0]\n"
  "  cbz   x9, 1b\n"
  // phase 2: does the virtual timer's PPI 27 reach this M3 at EL1? (Linux says yes under Starbite OS)
  "  DT 56\n"
  "  mov   w1, #0x80\n"
  "  strb  w1, [x20, #0x41B]\n"         // GICD_IPRIORITYR byte for INTID 27
  "  ldr   w0, [x20, #0x418]\n"
  "  DE 56\n"
  "  DT 57\n"
  "  mov   w1, #0x08000000\n"
  "  str   w1, [x20, #0x100]\n"         // GICD_ISENABLER0 (banked): INTID 27
  "  ldr   w0, [x20, #0x100]\n"
  "  DE 57\n"
  "  DT 58\n"
  "  str   xzr, [x19, #0x20]\n"
  "  mrs   x1, cntvct_el0\n"
  "  add   x1, x1, #2600\n"             // 100 us ahead
  "  msr   cntv_cval_el0, x1\n"
  "  mov   x1, #1\n"
  "  msr   cntv_ctl_el0, x1\n"
  "  isb\n"
  "  mov   x3, #0x40000\n"              // give up after ~10 ms
  "  msr   daifclr, #2\n"
  "3:\n"
  "  ldr   x0, [x19, #0x20]\n"
  "  cbnz  x0, 4f\n"
  "  mrs   x1, cntvct_el0\n"
  "  sub   x1, x1, x22\n"
  "  cmp   x1, x3\n"
  "  b.lo  3b\n"
  "4:\n"
  "  msr   daifset, #2\n"
  "  DE 58\n"
  "  DT 59\n"
  "  mrs   x0, cntv_ctl_el0\n"
  "  mov   x1, #2\n"
  "  msr   cntv_ctl_el0, x1\n"          // masked, off
  "  isb\n"
  "  DE 59\n"
  "  DT 60\n"
  "  mov   w1, #0x08000000\n"
  "  str   w1, [x20, #0x180]\n"         // GICD_ICENABLER0: INTID 27 off again
  "  mrs   x0, isr_el1\n"
  "  DE 60\n"
  "  mov   x21, #0xFD\n"
  "  str   x21, [x19]\n"
  "  dsb   sy\n"
  "5:\n"
  "  wfe\n"
  "  ldr   x9, [x19, #0xFC0]\n"
  "  cmp   x9, #2\n"
  "  b.lo  5b\n"
  // phase 3: CoreSight external debug of A55 core 0 (Samsung DT: cl0_cpu0 dbg 0x1E810000), read by this M3
  "  LDG 61, x28, 0xFF0\n"              // CIDR0
  "  LDG 62, x28, 0xFF4\n"              // CIDR1
  "  LDG 63, x28, 0xFE0\n"              // PIDR0
  "  LDG 64, x28, 0xFBC\n"              // EDDEVARCH
  "  LDG 65, x28, 0xFC8\n"              // EDDEVID (PCSample [3:0])
  "  LDG 66, x28, 0xFB4\n"              // EDLSR
  "  LDG 67, x28, 0x314\n"              // EDPRSR
  "  LDG 68, x28, 0xD00\n"              // MIDR
  "  LDG 69, x28, 0xD20\n"              // MPIDR[31:0]
  "  LDG 70, x28, 0x0A0\n"              // EDPCSR[31:0] (samples)
  "  LDG 71, x28, 0x0AC\n"              // EDPCSR[63:32]
  "  LDG 72, x28, 0x0A8\n"              // EDVIDSR
  "  LDG 73, x28, 0x0A0\n"
  "  LDG 74, x28, 0x0A0\n"
  "  DT 75\n"                           // software lock off, OS lock off (A55 0 only; it resets before Windows)
  "  movz  w1, #0xCE55\n"
  "  movk  w1, #0xC5AC, lsl #16\n"
  "  str   w1, [x28, #0xFB0]\n"
  "  dsb   sy\n"
  "  str   wzr, [x28, #0x300]\n"
  "  dsb   sy\n"
  "  isb\n"
  "  ldr   w0, [x28, #0xFB4]\n"
  "  DE 75\n"
  "  LDG 76, x28, 0x314\n"
  "  LDG 77, x28, 0x0A0\n"
  "  LDG 78, x28, 0x0AC\n"
  "  LDG 79, x28, 0x0A0\n"
  "  LDG 80, x28, 0x0A0\n"
  "  mov   x21, #0xFF\n"
  "  str   x21, [x19]\n"
  "  dsb   sy\n"
  "2:\n"
  "  wfe\n"
  "  b     2b\n"
  ".Ldirq:\n"
  "  ldr   w25, [x27, #0x0C]\n"         // GICC_IAR
  "  mov   x26, #2\n"
  "  msr   cntv_ctl_el0, x26\n"         // mask the timer before EOI
  "  isb\n"
  "  str   w25, [x27, #0x10]\n"         // GICC_EOIR
  "  add   x25, x25, #0x10000\n"
  "  str   x25, [x19, #0x20]\n"
  "  dsb   sy\n"
  "  eret\n"
  ".Ldexc:\n"
  "  mrs   x25, esr_el1\n"
  "  add   x26, x19, #0x40\n"
  "  add   x26, x26, x21, lsl #5\n"
  "  str   x25, [x26, #8]\n"
  "  mrs   x25, elr_el1\n"
  "  str   x25, [x26, #24]\n"
  "  ldr   x25, [x19, #8]\n"
  "  add   x25, x25, #1\n"
  "  str   x25, [x19, #8]\n"
  "  dsb   sy\n"
  "  mrs   x25, elr_el1\n"
  "  add   x25, x25, #4\n"
  "  msr   elr_el1, x25\n"
  "  eret\n"
  "  .p2align 3\n"
  "  .globl M3DiagLit\n"
  "M3DiagLit:\n"
  ".Ldlit:\n"
  "  .quad 0, 0, 0, 0\n"
  "  .globl M3DiagEnd\n"
  "M3DiagEnd:\n"
  );

STATIC CONST CHAR8  *mTestName[N_TESTS + 1] = {
  "-",        "midr",    "mpidr",   "revidr",  "pfr0",    "pfr1",    "isar0",   "isar1",
  "mmfr0",    "mmfr1",   "mmfr2",   "dfr0",    "afr0",    "ctr",     "clidr",   "dczid",
  "sctlr",    "cpacr",   "cntfrq",  "cntkctl", "ACTLR",   "ACTLRw",  "PMCR",    "PMCEID0",
  "PMUSEREN", "PMCCFILT","PMCCNTR", "PMCNTEN", "PMSELR",  "OSLSR",   "MDSCR",   "DBGBCR0",
  "ccsidr",   "DCCSW",   "DCCISW",  "ATs1e1r", "tlbi",    "ctxidr",  "ctxidrW", "mair",
  "mairW",    "isr",     "gicdCTLR","gicdTYPE","gicdIIDR","gicdEN0", "gicdTGT0","gicdPRI6",
  "gicdPRI7", "giccCTLR","giccPMR", "giccHPPI","giccIIDR","SMCCCver","PSCIver", "WA1feat",
  "pri6w",    "en0w",    "IRQ",     "vctl",    "isr2",
  "csCIDR0",  "csCIDR1", "csPIDR0", "csARCH",  "csDEVID", "csLSR",   "csPRSR",  "csMIDR",
  "csMPIDR",  "csPCSR",  "csPCSRhi","csVIDSR", "csPCSR",  "csPCSR",  "csUNLK",  "csPRSR",
  "csPCSR",   "csPCSRhi","csPCSR",  "csPCSR"
};

STATIC EFI_GUID  mDiagAcpi20Guid = {
  0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 }
};

STATIC CONST struct {
  UINT32    Offset, Mask, Value;
} mDiagPmu[] = {
  { 0x1088, 0xFF000000u, 0x8D000000u },
  { 0x2114, 3, 2 }, { 0x2180, 3, 2 }, { 0x21EC, 3, 2 }, { 0x2258, 3, 2 }, { 0x2384, 3, 2 },
};

STATIC EFI_PHYSICAL_ADDRESS    mDiagPage;          // data page; the blob (< 8 KiB) in the next two
STATIC UINTN                   mDiagGicd = GICD_FALLBACK;
STATIC UINTN                   mDiagGicc = GICC_FALLBACK;
STATIC EFI_EXIT_BOOT_SERVICES  mDiagOrigEbs;
STATIC EFI_EVENT               mDiagReadyToBoot;
STATIC BOOLEAN                 mDiagPreDone;
STATIC CHAR8                   mDiagSnap0[200];   // the dispatch snapshot, repeated after EBS

STATIC
UINT64
DiagSmc (
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
DiagCleanToPoc (
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

STATIC UINT16 DRd16 (CONST UINT8 *p) { UINT16 v; CopyMem (&v, p, 2); return v; }
STATIC UINT32 DRd32 (CONST UINT8 *p) { UINT32 v; CopyMem (&v, p, 4); return v; }
STATIC UINT64 DRd64 (CONST UINT8 *p) { UINT64 v; CopyMem (&v, p, 8); return v; }

//
// FNV-1a over a table: a change between two snapshots shows as a different hash.
//
STATIC
UINT32
DiagHash (
  IN CONST UINT8  *P,
  IN UINTN        Len
  )
{
  UINT32  H = 0x811C9DC5u;
  UINTN   i;

  for (i = 0; i < Len; i++) {
    H = (H ^ P[i]) * 0x01000193u;
  }

  return H;
}

STATIC
EFI_ACPI_DESCRIPTION_HEADER *
DiagXsdt (
  VOID
  )
{
  EFI_ACPI_2_0_ROOT_SYSTEM_DESCRIPTION_POINTER  *Rsdp;
  UINTN                                         i;

  Rsdp = NULL;
  for (i = 0; i < gST->NumberOfTableEntries; i++) {
    if (CompareGuid (&gST->ConfigurationTable[i].VendorGuid, &mDiagAcpi20Guid)) {
      Rsdp = gST->ConfigurationTable[i].VendorTable;
    }
  }

  if ((Rsdp == NULL) || (Rsdp->XsdtAddress == 0)) {
    return NULL;
  }

  return (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)Rsdp->XsdtAddress;
}

STATIC
UINT8 *
DiagFind (
  IN UINT32  Sig,
  OUT UINTN  *Count  OPTIONAL
  )
{
  EFI_ACPI_DESCRIPTION_HEADER  *Xsdt;
  EFI_ACPI_DESCRIPTION_HEADER  *T;
  UINT8                        *First;
  UINTN                        i;
  UINTN                        n;
  UINTN                        Hits;

  First = NULL;
  Hits  = 0;
  Xsdt  = DiagXsdt ();
  if (Xsdt != NULL) {
    n = (Xsdt->Length - sizeof (*Xsdt)) / 8;
    for (i = 0; i < n; i++) {
      T = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)DRd64 ((UINT8 *)(Xsdt + 1) + 8 * i);
      if ((T != NULL) && (T->Signature == Sig)) {
        Hits++;
        if (First == NULL) {
          First = (UINT8 *)T;
        }
      }
    }
  }

  if (Count != NULL) {
    *Count = Hits;
  }

  return First;
}

STATIC
UINT8 *
DiagDsdt (
  IN UINT8  *Fadt
  )
{
  UINT64  X;

  if (Fadt == NULL) {
    return NULL;
  }

  X = (DRd32 (Fadt + 4) >= 148) ? DRd64 (Fadt + 140) : 0;
  if (X == 0) {
    X = DRd32 (Fadt + 40);
  }

  return (UINT8 *)(UINTN)X;
}

//
// One line per checkpoint. FADT: +112 flags, +129 ARM_BOOT_ARCH (bit 0 PSCI compliant, bit 1 PSCI uses HVC),
// +131 minor revision, +268 hypervisor vendor identity (FADT 6.0+, 276 bytes).
//
STATIC
VOID
DiagAcpiSnap (
  IN CONST CHAR8  *Tag
  )
{
  EFI_ACPI_DESCRIPTION_HEADER  *Xsdt;
  UINT8                        *F;
  UINT8                        *M;
  UINT8                        *G;
  UINT8                        *D;
  UINTN                        nF;
  CHAR8                        Snap[190];

  Xsdt = DiagXsdt ();
  if (Xsdt == NULL) {
    SbLog ("=M3D acpi %a: no XSDT yet", Tag);
    return;
  }

  F = DiagFind (SIGNATURE_32 ('F', 'A', 'C', 'P'), &nF);
  M = DiagFind (SIGNATURE_32 ('A', 'P', 'I', 'C'), NULL);
  G = DiagFind (SIGNATURE_32 ('G', 'T', 'D', 'T'), NULL);
  D = DiagDsdt (F);
  AsciiSPrint (
    Snap,
    sizeof (Snap),
    " %a: X n%d %08x | F x%d %lx ba%x fl%x mr%d hv%lx %08x | A %lx l%d %08x | G %lx %08x | D %lx %08x",
    Tag,
    (Xsdt->Length - sizeof (*Xsdt)) / 8,
    DiagHash ((UINT8 *)Xsdt, Xsdt->Length),
    nF,
    (UINT64)(UINTN)F,
    F ? DRd16 (F + 129) : 0xFFFF,
    F ? DRd32 (F + 112) : 0,
    F ? F[131] : 0,
    (F && DRd32 (F + 4) >= 276) ? DRd64 (F + 268) : 0,
    F ? DiagHash (F, DRd32 (F + 4)) : 0,
    (UINT64)(UINTN)M,
    M ? DRd32 (M + 4) : 0,
    M ? DiagHash (M, DRd32 (M + 4)) : 0,
    (UINT64)(UINTN)G,
    G ? DiagHash (G, DRd32 (G + 4)) : 0,
    (UINT64)(UINTN)D,
    D ? DiagHash (D, DRd32 (D + 4)) : 0
    );
  SbLog ("=M3D acpi%a", Snap);
  if (AsciiStrCmp (Tag, "dispatch") == 0) {
    AsciiStrCpyS (mDiagSnap0, sizeof (mDiagSnap0), Snap);
  }
}

STATIC
VOID
DiagTableList (
  VOID
  )
{
  EFI_ACPI_DESCRIPTION_HEADER  *Xsdt;
  EFI_ACPI_DESCRIPTION_HEADER  *T;
  CHAR8                        Line[180];
  UINTN                        Pos;
  UINTN                        i;
  UINTN                        n;

  Xsdt = DiagXsdt ();
  if (Xsdt == NULL) {
    return;
  }

  n   = (Xsdt->Length - sizeof (*Xsdt)) / 8;
  Pos = 0;
  for (i = 0; i < n; i++) {
    T = (EFI_ACPI_DESCRIPTION_HEADER *)(UINTN)DRd64 ((UINT8 *)(Xsdt + 1) + 8 * i);
    if (T == NULL) {
      continue;
    }

    Pos += AsciiSPrint (
             Line + Pos,
             sizeof (Line) - Pos,
             " %c%c%c%c@%x/%x",
             (CHAR8)T->Signature,
             (CHAR8)(T->Signature >> 8),
             (CHAR8)(T->Signature >> 16),
             (CHAR8)(T->Signature >> 24),
             (UINT32)(UINTN)T,
             T->Length
             );
    if (Pos > sizeof (Line) - 24) {
      SbLog ("=M3D tables%a", Line);
      Pos = 0;
    }
  }

  if (Pos > 0) {
    SbLog ("=M3D tables%a", Line);
  }
}

STATIC
VOID
DiagHexDump (
  IN CONST CHAR8  *Name,
  IN CONST UINT8  *P,
  IN UINTN        Len
  )
{
  CHAR8  Line[140];
  UINTN  Off;
  UINTN  i;
  UINTN  Pos;

  if (P == NULL) {
    SbLog ("=M3D %a: none", Name);
    return;
  }

  for (Off = 0; Off < Len; Off += 64) {
    Pos = 0;
    for (i = Off; (i < Len) && (i < Off + 64); i++) {
      Pos += AsciiSPrint (Line + Pos, sizeof (Line) - Pos, "%02x", P[i]);
    }

    SbLog ("=M3D %a+%03x %a", Name, (UINT32)Off, Line);
  }
}

//
// Every GICC inside a DSDT _MAT: Windows takes processor entries from _MAT where present.
//
STATIC
VOID
DiagMatScan (
  IN UINT8  *Dsdt
  )
{
  UINT32  Len;
  UINT32  i;
  UINT32  j;
  UINT8   *G;
  UINTN   Found;

  if (Dsdt == NULL) {
    SbLog ("=M3D _MAT: no DSDT");
    return;
  }

  Len   = DRd32 (Dsdt + 4);
  Found = 0;
  for (i = 36; i + 4 < Len; i++) {
    if ((Dsdt[i] != '_') || (Dsdt[i + 1] != 'M') || (Dsdt[i + 2] != 'A') || (Dsdt[i + 3] != 'T')) {
      continue;
    }

    G = NULL;
    for (j = i + 4; (j < i + 20) && (j + 80 <= Len); j++) {
      if ((Dsdt[j] == 0x0B) && (Dsdt[j + 1] == 0x50)) {
        G = Dsdt + j;
        break;
      }
    }

    Found++;
    if (G == NULL) {
      SbLog ("=M3D _MAT @%x: no GICC buffer right after it", i);
      continue;
    }

    SbLog (
      "=M3D _MAT @%x: if=%x uid=%x flags=%x park v%x @%lx gicc %lx mpidr=%lx eff=%x",
      i,
      DRd32 (G + 4),
      DRd32 (G + 8),
      DRd32 (G + 12),
      DRd32 (G + 16),
      DRd64 (G + 24),
      DRd64 (G + 32),
      DRd64 (G + 68),
      G[76]
      );
  }

  SbLog ("=M3D _MAT: %d found in DSDT (len %x)", Found, Len);
}

//
// The MADT's GIC bases (GICD entry +8, first GICC +32), so the M3 reads the same frames Windows will use.
//
STATIC
VOID
DiagGicBases (
  VOID
  )
{
  UINT8   *M;
  UINT32  Len;
  UINT32  Off;
  UINT64  Gicc;

  M = DiagFind (SIGNATURE_32 ('A', 'P', 'I', 'C'), NULL);
  if (M == NULL) {
    SbLog ("=M3D gic: no MADT at dispatch, using the DT's %lx / %lx", (UINT64)mDiagGicd, (UINT64)mDiagGicc);
    return;
  }

  Len  = DRd32 (M + 4);
  Gicc = 0;
  for (Off = 44; Off + 2 <= Len && M[Off + 1] != 0; Off += M[Off + 1]) {
    if ((M[Off] == 0x0C) && (M[Off + 1] >= 16)) {
      mDiagGicd = (UINTN)DRd64 (M + Off + 8);
    } else if ((M[Off] == 0x0B) && (M[Off + 1] >= 40) && (Gicc == 0)) {
      Gicc = DRd64 (M + Off + 32);
    }
  }

  if (Gicc != 0) {
    mDiagGicc = (UINTN)Gicc;
  }

  SbLog ("=M3D gic: MADT says GICD %lx GICC %lx", (UINT64)mDiagGicd, (UINT64)mDiagGicc);
}

STATIC
VOID
DiagLogTests (
  IN UINTN  First,
  IN UINTN  Last
  )
{
  volatile UINT64  *R;
  CHAR8            Line[190];
  UINTN            Pos;
  UINTN            i;

  Pos = 0;
  for (i = First; i <= Last; i++) {
    R    = (volatile UINT64 *)(UINTN)(mDiagPage + D_REC + i * 0x20);
    Pos += AsciiSPrint (Line + Pos, sizeof (Line) - Pos, " %a=%lx", mTestName[i], R[0]);
    if (R[1] != 0) {
      Pos += AsciiSPrint (Line + Pos, sizeof (Line) - Pos, "!esr%x@%lx", (UINT32)R[1], R[3]);
    }

    if (R[2] > 4) {
      Pos += AsciiSPrint (Line + Pos, sizeof (Line) - Pos, "~%ld", R[2]);
    }

    if ((Pos > 110) || (i == Last)) {
      SbLog ("=M3D t%a", Line);
      Pos = 0;
    }
  }
}

STATIC
UINT64
DiagWait (
  IN  volatile UINT64  *P,
  IN  UINT64           Want,
  IN  UINTN            MaxMs,
  OUT UINTN            *Ms
  )
{
  for (*Ms = 0; *Ms < MaxMs; (*Ms)++) {
    DiagCleanToPoc ((UINTN)P, (UINTN)P + 0xF80);
    if (P[D_CRUMB / 8] == Want) {
      break;
    }

    MicroSecondDelay (1000);
  }

  return P[D_CRUMB / 8];
}

STATIC
VOID
DiagGo (
  IN volatile UINT64  *P,
  IN UINT64           Go
  )
{
  P[D_GO / 8] = Go;
  DiagCleanToPoc ((UINTN)P + D_GO, (UINTN)P + D_GO + 8);
  __asm__ __volatile__ ("sev" ::: "memory");
}

STATIC
VOID
DiagStopped (
  IN UINT64  Crumb
  )
{
  if ((Crumb >= 1) && (Crumb <= N_TESTS)) {
    DiagLogTests (1, (UINTN)Crumb);
    SbLog ("=M3D M3 core 2 STOPPED in test %ld (%a): that access never returned", Crumb, mTestName[Crumb]);
  } else {
    SbLog ("=M3D M3 core 2 never ran the stub (crumb %lx)", Crumb);
  }
}

STATIC
VOID
DiagM3 (
  VOID
  )
{
  volatile UINT32   *Pmu = (volatile UINT32 *)0x14060000UL;
  volatile UINT64   *P;
  UINT64            *Lit;
  UINT64            Ver;
  UINT64            Rc;
  UINT64            Crumb;
  UINTN             Blob;
  UINTN             i;
  UINTN             Ms;

  Ver = DiagSmc (0x84000000u, 0, 0) & 0xFFFFFFFFu;
  SbLog ("=M3D v2 at dispatch: A55 PSCI_VERSION %lx (FFFFFFFF = no shim yet, CPU_ON goes to Samsung EL3)", Ver);
  if (Ver != 0xFFFFFFFFu) {
    SbLog ("=M3D: the woa PSCI shim is already installed; no CPU_ON this boot (it would never reach our stub)");
    return;
  }

  mDiagPage = 0xFFFFFFFFu;
  if (EFI_ERROR (gBS->AllocatePages (AllocateMaxAddress, EfiReservedMemoryType, 3, &mDiagPage))) {
    SbLog ("=M3D: no pages");
    mDiagPage = 0;
    return;
  }

  Blob = (UINTN)mDiagPage + EFI_PAGE_SIZE;
  ASSERT ((UINTN)(M3DiagEnd - M3DiagBlob) <= 2 * EFI_PAGE_SIZE);
  ZeroMem ((VOID *)(UINTN)mDiagPage, 3 * EFI_PAGE_SIZE);
  CopyMem ((VOID *)Blob, M3DiagBlob, (UINTN)(M3DiagEnd - M3DiagBlob));
  Lit    = (UINT64 *)(Blob + (UINTN)(M3DiagLit - M3DiagBlob));
  Lit[0] = mDiagPage;
  Lit[1] = mDiagGicd;
  Lit[2] = mDiagGicc;
  Lit[3] = CS_A55_0;
  DiagCleanToPoc ((UINTN)mDiagPage, (UINTN)mDiagPage + 3 * EFI_PAGE_SIZE);
  __asm__ __volatile__ ("ic iallu\n dsb sy\n isb" ::: "memory");
  for (i = 0; i < ARRAY_SIZE (mDiagPmu); i++) {
    Pmu[mDiagPmu[i].Offset / 4] = (Pmu[mDiagPmu[i].Offset / 4] & ~mDiagPmu[i].Mask) | mDiagPmu[i].Value;
    __asm__ __volatile__ ("dsb sy" ::: "memory");
  }

  P  = (volatile UINT64 *)(UINTN)mDiagPage;
  Rc = DiagSmc (0xC4000003u, 0x102, Blob + (UINTN)(M3DiagEntry - M3DiagBlob));
  Crumb = DiagWait (P, 0xFE, 500, &Ms);
  SbLog (
    "=M3D M3 core 2: CPU_ON rc=%lx crumb %lx after %d ms (FE = all of phase 1), el %lx, EL1 exceptions %ld",
    Rc,
    Crumb,
    Ms,
    P[D_EL / 8] >> 2,
    P[D_EXC / 8]
    );
  if (Crumb != 0xFE) {
    DiagStopped (Crumb);
    return;
  }

  DiagLogTests (1, 55);

  //
  // Phase 2: the virtual timer's PPI 27 on the M3 (priority, enable, a 100 us timer, IRQ unmasked for <= 10 ms).
  //
  DiagGo (P, 1);
  Crumb = DiagWait (P, 0xFD, 200, &Ms);
  SbLog ("=M3D phase 2 (timer IRQ on the M3): crumb %lx after %d ms; IRQ = 1001B means INTID 27 arrived at EL1", Crumb, Ms);
  if (Crumb != 0xFD) {
    DiagStopped (Crumb);
    return;
  }

  DiagLogTests (56, 60);

  //
  // Phase 3: CoreSight external debug of A55 core 0, read from the M3 (MMU off); A55 0 is in this poll loop now.
  //
  SbLog (
    "=M3D phase 3: CoreSight of A55 0 at %lx; A55 0 polls in DiagWait %lx / MicroSecondDelay %lx (expect csPCSR near these)",
    (UINT64)CS_A55_0,
    (UINT64)(UINTN)DiagWait,
    (UINT64)(UINTN)MicroSecondDelay
    );
  DiagGo (P, 2);
  Crumb = DiagWait (P, 0xFF, 200, &Ms);
  SbLog ("=M3D phase 3: crumb %lx after %d ms (FF = done; a number = that register access hung the M3)", Crumb, Ms);
  DiagLogTests (61, (Crumb == 0xFF) ? 80 : ((Crumb >= 61 && Crumb <= 80) ? (UINTN)Crumb : 61));
}

//
// The A55's GIC and timer state right after ExitBootServices: what winload and the kernel inherit on this path.
//
STATIC
VOID
DiagA55State (
  VOID
  )
{
  volatile UINT32  *D = (volatile UINT32 *)mDiagGicd;
  volatile UINT32  *C = (volatile UINT32 *)mDiagGicc;
  UINT64           Daif, Isr, Vctl, Pctl, Kctl, Actlr, Pmcr, Oslsr, Mdscr, Mpidr, Sctlr, Tcr;

  __asm__ __volatile__ ("mrs %0, daif" : "=r" (Daif));
  __asm__ __volatile__ ("mrs %0, isr_el1" : "=r" (Isr));
  __asm__ __volatile__ ("mrs %0, cntv_ctl_el0" : "=r" (Vctl));
  __asm__ __volatile__ ("mrs %0, cntp_ctl_el0" : "=r" (Pctl));
  __asm__ __volatile__ ("mrs %0, cntkctl_el1" : "=r" (Kctl));
  __asm__ __volatile__ ("mrs %0, actlr_el1" : "=r" (Actlr));
  __asm__ __volatile__ ("mrs %0, pmcr_el0" : "=r" (Pmcr));
  __asm__ __volatile__ ("mrs %0, oslsr_el1" : "=r" (Oslsr));
  __asm__ __volatile__ ("mrs %0, mdscr_el1" : "=r" (Mdscr));
  __asm__ __volatile__ ("mrs %0, mpidr_el1" : "=r" (Mpidr));
  __asm__ __volatile__ ("mrs %0, sctlr_el1" : "=r" (Sctlr));
  __asm__ __volatile__ ("mrs %0, tcr_el1" : "=r" (Tcr));
  SbLog (
    "=M3D A55 after EBS: mpidr %lx daif %lx isr %lx cntv_ctl %lx cntp_ctl %lx cntkctl %lx actlr %lx pmcr %lx oslsr %lx mdscr %lx sctlr %lx tcr %lx",
    Mpidr, Daif, Isr, Vctl, Pctl, Kctl, Actlr, Pmcr, Oslsr, Mdscr, Sctlr, Tcr
    );
  SbLog (
    "=M3D A55 GIC after EBS: gicd ctlr %x typer %x en0 %x tgt0 %x pri6 %x pri7 %x | gicc ctlr %x pmr %x bpr %x hppir %x iidr %x",
    D[0], D[1], D[0x100 / 4], D[0x800 / 4], D[0x418 / 4], D[0x41C / 4],
    C[0], C[1], C[2], C[0x18 / 4], C[0xFC / 4]
    );
}

STATIC
EFI_STATUS
EFIAPI
DiagEbsWrap (
  IN EFI_HANDLE  ImageHandle,
  IN UINTN       MapKey
  )
{
  EFI_STATUS  Status;
  UINT8       *F;
  UINT8       *M;
  UINT8       *G;
  UINT64      Ver;

  if (!mDiagPreDone) {
    mDiagPreDone = TRUE;
    Ver          = DiagSmc (0x84000000u, 0, 0) & 0xFFFFFFFFu;
    SbLog ("=M3D EBS wrapper called from %lx; A55 PSCI_VERSION now %lx", (UINT64)(UINTN)__builtin_return_address (0), Ver);
    DiagAcpiSnap ("ebs-pre");
  }

  Status = mDiagOrigEbs (ImageHandle, MapKey);
  if (EFI_ERROR (Status)) {
    return Status;                      // stale map key: winload retries
  }

  mSbAfterEbs = TRUE;                   // gBS is gone: the log must not use it
  SbLog ("=M3D ExitBootServices returned %r (woa's hook included)", Status);
  DiagAcpiSnap ("ebs-post");
  DiagA55State ();
  F = DiagFind (SIGNATURE_32 ('F', 'A', 'C', 'P'), NULL);
  M = DiagFind (SIGNATURE_32 ('A', 'P', 'I', 'C'), NULL);
  G = DiagFind (SIGNATURE_32 ('G', 'T', 'D', 'T'), NULL);
  DiagHexDump ("FACP", F, F ? DRd32 (F + 4) : 0);
  (VOID)M;
  (VOID)G;
  DiagMatScan (DiagDsdt (F));
  SbLog ("=M3D (repeat, pmsg wraps) acpi%a", mDiagSnap0);
  SbLog ("=M3D v2 done. Windows' kernel was NOT started. Resetting now (warm); if that fails the watchdog resets in ~2 min");
  gST->RuntimeServices->ResetSystem (EfiResetWarm, EFI_SUCCESS, 0, NULL);
  SbLog ("=M3D ResetSystem returned; waiting for the watchdog");
  for ( ; ;) {
    __asm__ __volatile__ ("msr daifset, #0xf\n wfi" ::: "memory");
  }

  return Status;
}

STATIC
VOID
EFIAPI
DiagOnReadyToBoot (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  EFI_STATUS  Status;

  DiagAcpiSnap ("ready-to-boot");
  if (gBS->ExitBootServices == DiagEbsWrap) {
    return;
  }

  mDiagOrigEbs          = gBS->ExitBootServices;
  gBS->ExitBootServices = DiagEbsWrap;
  gBS->Hdr.CRC32        = 0;
  Status                = gBS->CalculateCrc32 ((UINT8 *)gBS, gBS->Hdr.HeaderSize, &gBS->Hdr.CRC32);
  SbLog ("=M3D ReadyToBoot: ExitBootServices %lx wrapped (outermost) by %lx (crc %r)", (UINT64)(UINTN)mDiagOrigEbs, (UINT64)(UINTN)DiagEbsWrap, Status);
}

VOID
M3DiagArm (
  VOID
  )
{
  EFI_STATUS  Status;

  SbLog ("=M3D v2 armed: log only; M3 core 2: instruction probe, timer IRQ, CoreSight of A55 0; reset after ExitBootServices");
  DiagAcpiSnap ("dispatch");
  DiagTableList ();
  DiagGicBases ();
  DiagM3 ();
  Status = gBS->CreateEventEx (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, DiagOnReadyToBoot, NULL, &gEfiEventReadyToBootGuid, &mDiagReadyToBoot);
  SbLog ("=M3D ReadyToBoot event %r", Status);
}

#endif
