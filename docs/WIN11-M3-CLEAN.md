# Windows 11 on the M3 big cores only: clean-slate track (started 2026-11-10)

Goal: Windows 11 ARM64 (23H2, 22631.7584) on the four Exynos M3 cores (MPIDR 0x80000100..0x80000103) and
nothing else. Never mixed with the A55s (LSE on the A55s only; a mixed set is 0x3E or worse).

Rules of this track: fix the platform in our firmware (ACPI tables, PSCI provider, CPU/GIC/timer state); never
patch, hook or modify ntoskrnl, hal, winload or any Windows file or in-memory image. If a Windows-side change ever
looks unavoidable, stop and ask the main session first. If a safety check stops a step: stop that route, report it.

The old track (solo1..solo10, big3/big4, `build/woa/cores/` images, WIN11-BIGCORES*.md on branch
claude/laughing-pare-2ca469) is data here, not a plan. Its build tooling (tools/woa, starbite/s9usb) is reused.

## 1. What Windows needs from a platform before it runs on a set of CPUs

| Item | What Windows ARM64 expects | Linux on the M3s today (Starbite OS guest) | Windows on the old M3 path |
|---|---|---|---|
| Boot CPU | Whatever CPU runs UEFI and winload; the kernel takes the boot CPU's features (ID_AA64*) as the system's | the M3 is the boot CPU from its first instruction (CPU_ON from Starbite OS) | winload and all of UEFI ran on A55 0; the M3 took over **after** ExitBootServices, in the middle of winload |
| MADT GICC | one enabled entry per CPU, boot CPU first; MPIDR, CPU interface number, GICC base; parking mailbox if the parking protocol is used | DT: 4 M3 cpus, `enable-method = "psci"` 0.1 node | M3 1 first (UID 5, if 5, MPIDR 0x101), the rest disabled |
| FADT ARM_BOOT_ARCH | bit 0 PSCI_COMPLIANT (CPU_ON for secondaries), else the ACPI parking protocol | n/a (DT) | **live value 0** (solo9), although the woa source (Fadt.aslc) and the static table say 1; so Windows uses parking, not PSCI, on both paths |
| PSCI | only if ARM_BOOT_ARCH says so; reset/off can come from the FADT reset register (woa: PMU SWRESET) | PSCI 0.1 from Samsung EL3 (`cpu_on`, `cpu_off`, no PSCI_VERSION) | EL3 answers PSCI_VERSION with FFFFFFFF on the M3 (solo7); the A55 sees 0x10000 only through woa's EL2 shim |
| GTDT | the generic timer PPIs; Windows uses the virtual timer (PPI 27) | PPI 27 at EL1, works on the M3 (P1b..P4) | same table |
| CNTFRQ_EL0 | the counter frequency | 0 on every core (S-Boot never sets it) | woa's BdsDxe patches three `mrs cntfrq` sites in ntoskrnl to a 26 MHz stub (pre-existing, CPU-independent) |
| GIC (GIC-400, v2) | GICD + GICC frames, the CPU interface number per core, PPIs/SGIs Group 1 | interface bits 0x20/0x40 on M3 1/2 (P1b); shared-distributor patch on the Linux side | B3a: M3 n = interface 4 + n, as the MADT says |
| PPTT | optional; must name the enabled UIDs | n/a | release PPTT has leaves 0..7 |
| EL2 | anything Windows executes at EL1 must either run natively or be served by EL2 | whatever EL3 sets on an M3 warm boot: proven fine for what Linux executes | same EL3 defaults; A55 0 alone has woa's payload (HCR = RW + TSC + SWIO) |

## 2. What the old track established (re-checked, with corrections)

1. B1/B2/B2b/B3a: the M3s start from UEFI dispatch with the six PMU writes, park in WFE for a whole Windows
   session, and run UEFI's page tables coherently with the A55.
2. solo4..solo10: M3 core 1 becomes the boot CPU after ExitBootServices, returns into winload (=EBSOK printed,
   `prog 02 catch 00`), then a silent hang until the watchdog (~2 min). **Correction:** "silent" only means no
   =UFS line. The =UFSBUGCHECK marker needs Windows' storage stack; a bugcheck in kernel phase 0 or early phase 1
   cannot produce it. The M3's catch vectors also only cover the time until winload/the kernel load their own
   VBAR. So "no exception" is proven only for the first moments after the hand-off.
3. solo9 is the only clean PSCI data point (solo10's emulator overlapped woa's CNTFRQ stub; research note 1.1),
   and the live ARM_BOOT_ARCH was already 0. So **Windows never used PSCI CPU_ON on either path**: missing PSCI is
   not the blocker for a one-CPU boot.
4. "Linux runs on the M3 under this firmware" is **not quite true**: it runs under Starbite OS (our own kernel,
   no UEFI, no woa shim), started with PSCI 0.1 CPU_ON straight to Samsung EL3, entry below 4 GiB, and the Linux
   kernel carries two small Starbite patches (shared GIC distributor; CPU_ON trampoline below 4 GiB). Its value
   here is the list of EL1 operations it proves safe on an M3 under EL3's warm-boot EL2 defaults: WFI, the
   virtual timer and PPI 27, GICv2 CPU interface, TTBR/TCR/SCTLR/MAIR writes, TLBI, DC by VA/ZVA, ID registers,
   CCSIDR/CLIDR (cacheinfo), OSLAR and MDSCR (debug-monitors), DBGB*/DBGW* (hw_breakpoint reset), PMUSERENR
   (__cpu_setup), SMC to EL3.
5. M3 core 0 is startable: Linux P2b..P4 start at 0x100 with the PMU defaults plus FlexPMU's cluster/core
   bookkeeping. woa's "CPU_ON 0x100 takes the SoC down" was without them.

## 3. Ranked causes of the hang (static analysis, 2026-11-10)

Census: `llvm-objdump` of ntoskrnl 22621.7582 (build/win/xta-x1), every system instruction, compared with the
Linux list above. Windows-only on an M3 (sites): ACTLR_EL1 read (2) and write (1); set/way `DC CSW` (6) and
`DC CISW` (2); PMU beyond PMUSERENR: PMCCNTR_EL0 (493 reads), PMCR, PMSELR/PMXEVCNTR, PMCEID, PMCCFILTR,
PMEVTYPERn, PMINTEN; `HVC #0` (19) and `HVC #imm` (3); OSLSR; `AT S1E1R/S1E0R`; SMCCC_ARCH_WORKAROUND_1 SMCs in the
exception stubs; plus feature-gated ones (PAC keys, SSBS, ICC_* for GICv3, CNTHP_* for VHE) that an ARMv8.0 GICv2
CPU should never reach.

| # | Cause | Why it fits | Why the A55 path is fine | Evidence that confirms it | Firmware-side fix |
|---|---|---|---|---|---|
| 1 | **An EL1 instruction Windows runs (and Linux never does) traps to an EL2 nobody serves on the M3**: the core vanishes with no EL1 exception, no bugcheck | matches "catch 00, nothing logged, watchdog"; candidates above, ACTLR (HCR_EL2.TACR) and set/way (TSW) first, then PMCR (MDCR_EL2.TPMCR), HVC | A55 0 has woa's payload at EL2; A55 secondaries run with EL3's A55 warm-boot defaults, which may differ from the M3 cluster's | **m3diag1**: the probe's crumb stops at that test on M3 core 2; or a large tick count = trapped and emulated | own the M3's EL2 ourselves: EL3's SiP 0xC2000400 installs an EL2 payload **once per boot** (solo8: refused for a second core). Install ours from M3 core 1 *instead of* woa's from A55 0; an M3-only Windows doesn't need the A55's shim |
| 2 | **Boot-CPU state decided on the A55 before the hand-off** (winload ran on A55 0 until ExitBootServices): feature latches (the LSE one was real), translation settings (A55 has HAFDBS, the M3 none: hardware access-flag/dirty assumptions), cache geometry, the GIC/timer setup UEFI left on A55 0 | the hang lies after winload resumed on the M3, which is exactly where A55-made decisions start to matter | on the A55 path nothing changes CPU mid-boot | m3diag1 logs the M3's ID registers next to the A55 state at EBS; a static look at winload's kernel-transfer TCR value | **move the hand-off before winload starts** (ReadyToBoot, or better: UEFI on the M3 from early DXE), so every boot-CPU decision is made on an M3. The standards-shaped answer: Windows boots on whatever CPU UEFI runs on |
| 3 | Early kernel bugcheck that never reaches the screen or pmsg (e.g. 0x5C/0x7E on a missing timer interrupt, a MADT/PPTT/topology mismatch for an M3-only set) | phase-0 bugchecks are invisible here (no storage stack, our vectors replaced) | the A55 tables were tuned on the A55 path | full live FADT/MADT/GTDT/_MAT dumps (m3diag1); later a CoreSight PC sample of the hung M3 (below) | correct tables: M3 entries only, boot M3 first, PPTT naming exactly those UIDs, efficiency classes equal |
| 4 | Timer/interrupts not delivered to the M3 (PPI 27 group/enable/priority in the M3's banked registers, GICC state) | the kernel waits on the clock tick very early | Linux proves PPI 27 on the M3 under Starbite OS, so lower | m3diag1 tests 42..52 (the M3's banked GIC view) vs the A55's GIC line after EBS | program the M3's banked GIC state in firmware before handing off |
| 5 | PSCI / SMCCC answers on the M3 (FFFFFFFF) | Windows probes PSCI and SMCCC early | the A55 0 payload answers 0x10000 | m3diag1 tests 53..55; ARM_BOOT_ARCH is 0 live anyway | our own EL2 PSCI provider (from #1) answering PSCI 1.0 + SMCCC 1.1 |
| 6 | The unknown module that clears ARM_BOOT_ARCH, if it also changes other FADT/MADT fields Windows relies on | solo9 saw 0 where the source has 1 | same tables | m3diag1's four snapshots (dispatch, ReadyToBoot, before and after the woa EBS hook) bracket the writer | install the tables we want after it, or replace its input |

Lower: CNTFRQ (woa's patch is CPU-independent), cache coherency between clusters (B3a both ways ok), the GIC
interface numbers (B3a: 4 + n, as listed).

## 4. Image m3diag1 (log only; Windows' kernel never starts)

`starbite-uefi-m3diag1.bin`, sha256 `57ce03a08085e548b53e60c60dc868f1b7e0d5ff8790965738872be4cb58cf0e`
(`SOLO=m3diag1 M3DIAG=1 tools/woa/build-win-usbhost.sh`; src38 + Star2LteSsdtDxe with only `M3Diag.c`, checked by
dxe-diff: "B = A + added files"). Code: `tools/woa/modules/Star2LteSsdtDxe/M3Diag.c`.

What it does:
1. At dispatch (A55 PSCI_VERSION must still read FFFFFFFF, i.e. before woa's shim; otherwise no CPU_ON): the six
   PMU defaults, **one** CPU_ON of M3 core 2 (0x102, as the old timer probe) into a stub with MMU off and its own
   EL1 vectors. The stub runs 55 tests one by one (ID registers, ACTLR read + write-back, PMU registers, OS lock,
   MDSCR, DBGBCR0, CCSIDR, DC CSW/CISW on L1 set 0 way 0, AT, TLBI, CONTEXTIDR/MAIR write-back, the M3's banked GIC
   distributor/CPU-interface view, SMCCC_VERSION, PSCI_VERSION, SMCCC_ARCH_FEATURES(WORKAROUND_1)), writing a crumb
   before each, then value, ESR of any EL1 exception (caught and skipped) and the duration in 26 MHz ticks. After
   the A55 logged phase 1, an `HVC #0` (SMCCC_VERSION) runs as phase 2.
2. ACPI snapshots at dispatch, ReadyToBoot, ExitBootServices before and after woa's hook.
3. After ExitBootServices: the A55's GIC/timer/system-register state, hex dumps of the live FADT, MADT, GTDT, every
   DSDT `_MAT`, then `ResetSystem(warm)`. If that returns, WFI until the loader's watchdog (~2 min).

### Run (main session; user present; the M3 CPU_ON is the only risk)

1. The hidden row is not in today's catalog. Re-add it (as before, dbde8cd/9c4de1b):
   ```
   [[entry]]
   key = "win11-m3probe"
   name = "Windows 11 (big-core probe)"
   version = "m3diag1: log only, resets before the Windows kernel"
   category = "lab"
   method = "uefi"
   tag = "lab"
   files = ["win11-m3probe.uefi"]
   root = "ufs:WINDOWS (NTFS)"
   flags = ["hidden"]
   ```
   payload `win11-m3probe.uefi` = `build/woa/cores/starbite-uefi-m3diag1.bin` (check the sha256 above).
2. Boot key `win11-m3probe` (flash && boot chained). Nothing plugged in.
3. Expect: Samsung logo, possibly the Windows boot logo for a moment, then a reset back to the loader menu within
   about 10 s. If the screen stays on the Samsung logo for more than 30 s: the M3 CPU_ON hung (never seen in 9+
   starts since B1) or the reset failed; wait 2 min for the watchdog, else force reset. Either is before Windows'
   kernel, so the Windows volume and registry are never written by Windows.
4. At the loader menu, **before TWRP**: `fastboot oem log dmesg` (zone 0), save it; grep `=M3D`. pmsg in TWRP holds
   the same lines as a backup.
5. If Windows' next normal boot shows "Windows didn't start correctly", choose to start normally (the boot manager
   saw an interrupted boot, as after every old solo hang).

### Reading it

| Line | Pass | Meaning otherwise |
|---|---|---|
| `=M3D v1 at dispatch: A55 PSCI_VERSION FFFFFFFF` | FFFFFFFF | 10000: the shim was already in; no probe this boot (tell me) |
| `=M3D M3 core 2: CPU_ON rc=0 crumb FE ...` | FE, el 1 | a number N = **test N trapped to an EL2 that never returned: cause #1 found** (the STOPPED line names it) |
| `=M3D t name=value[!esrXXXX@elr][~ticks]` | no `!esr`, no large `~` | `!esr` = undefined at EL1 (EC in ESR[31:26]); `~` far above the reads around it (tens+) = trapped to EL2 and emulated |
| `=M3D phase 2: crumb FF` | FF: HVC returned (value = whoever is at EL2) | 38 = HVC lost the core |
| `=M3D acpi <tag>: ... ba<x> ...` | four lines | the first tag where `ba` turns 0 brackets the FADT writer; changing hashes show which tables move when |
| `=M3D FACP/APIC/GTDT+...`, `=M3D _MAT ...` | present | the exact tables Windows receives |
| `=M3D v1 done` | present | missing: the run stopped earlier (send the whole zone 0) |

## 5. Next steps (after m3diag1)

- If a test stops the M3 (#1): an EL2 of our own on the boot M3 (SiP 0xC2000400 from the M3 before woa's shim
  uses it on A55 0), minimal: HCR_EL2 with no traps but TSC for PSCI 1.0 / SMCCC 1.1 answers. Prove it with a
  log-only image first.
- Either way (#2): move the boot CPU to M3 1 before winload (ReadyToBoot first, early DXE later), log-only image
  that resets at ExitBootServices, so UEFI's timer tick, GOP and winload's loading all run on the M3 first.
- To see a later hang without touching Windows: sample the hung M3's PC from an A55 watcher through CoreSight
  (Samsung's DT: debug base 0x1E000000 + 0x410000 + n * 0x100000 for M3 core n; EDPCSR). Accessibility (OS lock,
  power, NIDEN) has to be probed first in a log-only image.
- Tables for M3-only: M3 1 first, M3 0/2/3 parked (parking v1) with the FlexPMU core/cluster steps Starbite OS
  uses for core 0, A55s disabled and parked in WFI, PPTT with UIDs 4..7, equal efficiency classes.

## 6. m3diag1 result (2026-11-10 19:58, attended, PASSED)

Log: `build/woa/cores/m3diag1-run1/pstore-pmsg-ramoops-0.txt` (main checkout; pmsg read in TWRP, because the warm
reset landed in TWRP, not the menu; the pmsg ring wrapped and lost the first `=M3D` lines, so v2 repeats the
dispatch snapshot at the end and drops most hex dumps).

- **Cause #1 is out for every listed instruction.** M3 core 2: `crumb FE after 1 ms, el 1, EL1 exceptions 0`. ACTLR read
  and write-back, PMCR/PMCEID0/PMUSERENR/PMCCFILTR/PMCCNTR/PMCNTENSET/PMSELR, OSLSR (A = OS lock set), MDSCR, DBGBCR0,
  CCSIDR, DC CSW/CISW, AT, TLBI, CONTEXTIDR/MAIR write-back, GIC reads: all native (6..21 ticks; TLBI 50, first GICD
  read 51, SMCs 49..157). Nothing was emulated by an EL2.
- SMCCC_VERSION, PSCI_VERSION, SMCCC_ARCH_FEATURES from the M3: FFFFFFFF (Samsung EL3, PSCI 0.1).
- **HVC is UNDEFINED at EL1 on the M3** (`!esr2000000`, EC 0): EL2 has HVC disabled (HCR_EL2.HCD or SCR). On A55 0
  the woa payload swallows HVCs. ntoskrnl's HVC sites are a generic SMCCC-over-HVC wrapper (0x140200060, one caller)
  and conduit pairs (`hvc;ret` / `smc;ret`), plus Hyper-V hypercall stubs: all selected by FADT PSCI_USE_HVC /
  hypervisor presence, and FADT has neither. So a bare HVC on the boot path is unlikely; noted, not ranked up.
- M3 IDs as in B3a (MIDR 531F0020, ISAR0 11120, MMFR1 0, PFR1 0, DFR0 10305116, CTR 84448004, CLIDR B200123,
  DCZID 4); PMCR 53023000 (6 counters).
- M3's banked GIC view at dispatch: GICD_CTLR 1, ISENABLER0 7FFF (SGIs only), ITARGETSR0 40404040 (interface 6 =
  M3 2, as expected), IPRIORITYR6/7 = 0 (A55 0: 80808000/80808080, UEFI's), GICC_CTLR 1, PMR F0.
- Live tables (same at ReadyToBoot, ebs-pre, ebs-post; the dispatch line was lost):
  - FADT 276 bytes, flags 0x300400 (HW_REDUCED, LOW_POWER_S0, RESET_REG_SUP), reset register = memory 0x14060400
    (PMU SWRESET) value 1, **ARM_BOOT_ARCH 0** (no PSCI, no HVC), minor 3, no hypervisor vendor.
  - MADT 708 bytes: GICC UID 0..3 = A55, interfaces 0..3, enabled, MPIDR **0, 1, 2, 3** (the real A55 affinities are
    0x000, 0x100, 0x200, 0x300), UID 0 parking v0, UIDs 1..3 parking v1 at BBC60000/61000/62000, efficiency class 1;
    UID 4..7 = M3, interfaces 4..7, **disabled**, MPIDR 0x100..0x103, class 0; GICD 0x10101000 v2.
  - GTDT: secure EL1 PPI 29, non-secure EL1 PPI 30, virtual PPI 27, EL2 PPI 26, all flags 6 (level, active low,
    always-on); no memory-mapped counter.
  - DSDT `_MAT`: **all eight processors enabled**, parking v0 at 0, M3 class 0, A55 class 1. The old solo builds did
    not sync `_MAT` (only big mode did), so on the solo path Windows saw a MADT with one M3 and a DSDT with eight
    enabled processors whose `_MAT` points at A55 0..3 and M3 0..3: a real inconsistency to fix in our tables.
- A55 0 after ExitBootServices: GICD and GICC disabled (CTLR 0, PMR 0, UEFI's GIC driver), timers off, DAIF 2C0,
  SCTLR 30C5183D, TCR 280803518.

Updated ranking: #1 (unserved EL2 trap) is unlikely now; #2 (boot-CPU state made on the A55 before the hand-off)
and #3 (an invisible early bugcheck, e.g. from the MADT/`_MAT` mismatch or a missing clock interrupt) lead. Seeing
the hang's PC is the next lever, which m3diag2 prepares.

## 7. Image m3diag2 (log only; Windows' kernel never starts)

`starbite-uefi-m3diag2.bin`, sha256 `8996c62a285e51b300b668738244fcfc60ac08c2f0094e697ad77adaa011e733`
(`SOLO=m3diag2 M3DIAG=1`). Phase 1 as v1 without HVC; then, each after the A55 logged the previous phase:
- **phase 2 (timer interrupt on the M3):** priority 0x80 for INTID 27, enable it in the M3's banked ISENABLER0, the
  virtual timer 100 us ahead, IRQ unmasked for up to 10 ms. `IRQ=1001B` = INTID 27 reached EL1 on the M3 (IAR +
  0x10000); `IRQ=0` = never arrived (then cause #4 is real under UEFI's GIC setup); crumb stuck at 58 = the IRQ went
  to an EL2 that never returned.
- **phase 3 (CoreSight PC sampling):** the M3 (MMU off) reads A55 core 0's external debug block (0x1E810000, from
  Samsung's DT): CIDR/PIDR/EDDEVARCH/EDDEVID/EDLSR/EDPRSR/MIDR/MPIDR, EDPCSR samples; then unlocks (EDLAR
  C5ACCE55, OSLAR 0) and samples again. A55 0 is polling in `DiagWait`/`MicroSecondDelay` at that moment, and their
  addresses are logged: a `csPCSR` close to them proves we can sample a hung core's PC from another core with no
  change to Windows. `!esr...` = the access aborted (caught); crumb stuck at 61..80 = that access hung the M3 (the
  A55 carries on and the image still resets).

Run exactly as m3diag1 (row `win11-m3probe`, attended, reads from pmsg in TWRP or zone 0). Pass = `=M3D v2 done`.

## 8. m3diag2 result (20:12, attended, PASSED) and m3diag3

- **Timer interrupt on the M3: works.** INTID 27 got priority 0x80 and its banked enable bit (en0w 8007FFF). IAR
  reads 0x1B after 2711 ticks (≈104 us for a 100 us timer), and the M3 got it at EL1 under EL3's defaults. Cause #4 out.
- **The FADT/MADT writer is bracketed:** at our dispatch ARM_BOOT_ARCH = **1** (FADT hash 52CFD2E9, MADT AF2D3771),
  and by ReadyToBoot it is **0** (E5D67277) with the MADT changed too (A105FFF5). The writer runs between
  Star2LteSsdtDxe's dispatch and ReadyToBoot: the woa PSCI EL2 shim (it logs `acpi=1` after installing) or the
  release BDS's "P" step. Both are woa's firmware, not Windows. The tables we hand Windows on the M3 path must be
  written after it (at ReadyToBoot or EBS, as before), complete: MADT **and** DSDT `_MAT`.
- **CoreSight:** 0x1E810000 is A55 0's ARMv8.0 debug block (CIDR 0D/90, EDDEVARCH 47708A15, MIDR 410FD051). EDPRSR
  2AB / 2A1 = powered, **OS lock set**. In v8.0 the OS lock blocks EDPCSR, which matches every PCSR read giving a sync
  external abort (96000610). The external EDLAR/OSLAR writes were refused with an SError (BE000000).
- **m3diag3** (`starbite-uefi-m3diag3.bin`, sha256 `4843ffeb09f62fbf2730cbc9e93ebd59a68494eed38426f55921f4cbe149b9f0`):
  as v2, but A55 0 clears **its own** OS lock (`msr OSLAR_EL1, xzr`, as Linux and Windows do at boot; OSLSR logged
  before/after) before phase 3. No external debug writes at all. Test 75 reads EDCIDSR instead. Pass: csPCSR values
  near the logged DiagWait / MicroSecondDelay addresses (or 0xFFFFFFFF = sampling prohibited by the SoC's
  authentication signals). If it works, the hand-off image can have M3 core 2 (MMU off, outside Windows) sample M3
  core 1's PC every few ms. M3 1 would clear its own OS lock in our hand-off code. That shows where Windows stops,
  without touching Windows.

Next (being built): the boot CPU moves to M3 1 **before winload** (at ReadyToBoot), and UEFI's GIC/timer banked
state is replicated on it (INTID 27/30 priority and enable, GICC CTLR/PMR/BPR, CNTV/CNTP). UEFI and winload then
run on the M3 up to ExitBootServices, and the image resets there (log-only). Then the real boot with corrected tables.

## 9. m3diag3 result (20:53, PASSED) and m3early1: the boot CPU moves to an M3 before winload

m3diag3: A55 0 cleared its own OS lock (OSLSR A -> 8), every CoreSight read completed, but EDPCSR/EDCIDSR/EDVIDSR
read 0 and **EDDEVID = 0: PC sampling is not implemented in the A55's debug block.** Dropped. (The M3's own debug
block, 0x1E410000 + n * 0x100000, could differ; not pursued now.)

**m3early1** (`starbite-uefi-m3early1.bin`, sha256 `5844614aad1df9733ddc21ae8617999ccff01fc22210a7a52eb4ea2c5d6bcb61`,
`SOLO=m3early1 M3EARLY=1`; code `M3Early.c`). Log only:
- dispatch: M3 core 1 parked (one CPU_ON with the PMU defaults, as B3 v3..solo10);
- ReadyToBoot (our callback, TPL HIGH): the A55's banked GIC state (ISENABLER0, IPRIORITYR0..7), CPU interface
  (CTLR/PMR/BPR) and timers (CNTP/CNTV CTL+CVAL) saved; the EL1 context handed to the M3 (M3Boot's proven
  mechanism); on the M3 the GIC state is applied to its own interface, SPIs aimed at interface 0 are retargeted to
  interface 5, timers restored, TPL back. Then a 10 ms UEFI timer event proves the tick runs on the M3;
- A55 0 masks its timers and becomes a watcher: it rewrites the `=M3E watch beat .... prog .. pmsg ....` line in both
  rings every 0.5 s for 90 s (prog 1 claimed, 2 GIC/timer moved, 3 tick seen / 33 not, 4 left ReadyToBoot,
  5 in ExitBootServices, 6 EBS done), then WFI with MMU off;
- the boot manager and winload load and run on the M3; at ExitBootServices (our wrapper, outermost) the M3 logs its
  MPIDR, calls the real EBS (woa's hook included), logs, and resets (ResetSystem, then PMU SWRESET).
Fail-safe: no claim within 1 s, then the A55 stays the boot CPU and the image still resets at EBS.

What it answers: whether UEFI and winload work on an M3 boot CPU. That is cause #2 tested at the right layer: after
this, every decision winload makes is made on the M3.

## 10. m3early1 result (21:33, PASSED) and m3kern1: the first kernel attempt on the M3

m3early1: `now on mpidr 80000101 (el 1)`, A55 GIC en0 6C007FFF (SGIs + PPIs 26/27/29/30), pri7 80808080, CPU
interface ctlr 1 pmr F0 bpr 7, 479 SPIs moved to interface 5, CNTP on (UEFI's tick), and `a 10 ms timer event FIRED
after 3 ms`. The boot manager and winload then ran on the M3; `ExitBootServices ... running on mpidr 80000101`,
=EBSOK, `returned Success on mpidr 80000101`, done. **UEFI, the Windows Boot Manager and winload work on an M3 boot
CPU.** (The watch line showed prog 04 because the reset came within its 0.5 s period.)

**m3kern1** (`starbite-uefi-m3kern1.bin`, sha256 `31c0ebd387acb826c4f781eb04baf684338d5140f87b5c9bd938829b1408670b`,
`SOLO=m3kern1 M3EARLY=2`): m3early1, plus:
- at ExitBootServices, on the M3, after woa's rewrite: MADT with UID 5 (M3 1, MPIDR 0x101, interface 5) as the first
  and only enabled GICC, no mailbox; every DSDT `_MAT` copy synced (only UID 5 enabled); checksums fixed; FADT left
  as woa makes it (ARM_BOOT_ARCH 0: parking protocol, no PSCI). Then the M3 **returns into winload** and Windows'
  kernel starts on the M3 (1 CPU). No Windows file or memory is changed by us (woa's existing CNTFRQ/PMU patches
  in its EBS hook are unchanged, as on every A55 boot);
- **the volume guard (outside Windows):** A55 0 (not in Windows' MADT, interrupts masked) jumps into reserved memory
  with its own page tables and polls the pmsg ring header. The first line Windows' re-signed UFS driver logs as it
  loads (=UFSENTRY/=UFSINIT/=UFSNORMAL, always before its first =UFSWRIT) changes the header. 2 ms later A55 0
  cleans the ring and writes the FADT reset register (PMU SWRESET), the same reset Windows itself uses. With no
  change for 90 s (kernel hung before storage), it resets the same way.

Residual risk: if the UFS driver ever issued a disk write before its first pmsg line, up to ~2 ms of I/O could happen
before the reset. Its logs show =UFSNORMAL before every =UFSWRIT, and file-system/registry writes come much later
(volume mount, then hive flushes after smss), so the reset lands long before the registry is touched.

## 11. m3kern1 result (21:45, no write) and m3kern2

m3kern1: every =M3E line up to "returning into winload on the M3" (zone 0 at the menu). Then no =UFS line, no
=UFSBUGCHECK: the kernel never reached the storage driver. The loader's own row watchdog (82 s) reset first ("HANG
(WATCHDOG)" at ~91 s), before the guard's 90 s fallback. Nothing was written (registry backup
backup/20261110-pre-m3kern1).

**m3kern2** (`starbite-uefi-m3kern2.bin`, sha256 `bec3abbd763ded7123903f062410639d84e8367e617fde13eedf41b927dfb9d2`):
m3kern1 + the guard records how far the kernel got, from outside Windows, every 0.5 s into a 42-character line
(fits the menu's ~70-char `oem log dmesg` print), in zone 0 and pmsg:

`=M3K bBBBB eEE gdG @GG fbnNN @FF hHHHHHHHH`
- `b` beats (0.5 s) since the kernel was entered;
- `e` how many GICD_ISENABLER words 1..15 are non-zero (Windows enabling SPIs as the HAL/drivers come up);
- `gd` GICD_CTLR enable bit now, `@` the beat it first read 1 (Windows' HAL re-enabled the distributor, which
  UEFI's ExitBootServices had turned off). gd0 throughout = the kernel never got to HAL interrupt init;
- `fbn` how many times a sampled band of the framebuffer (lower middle: the boot spinner's area) changed, `@` the
  beat of the first change; `h` the last hash. Many changes = the spinner animates (kernel phase 1 running with a
  working clock); 1-2 = a static redraw (e.g. a bugcheck screen); 0 = the screen never changed after winload.
Fallback reset 120 s (the loader's 82 s watchdog comes first and lands in the menu, where zone 0 is readable).

## 12. m3kern2 result (22:02) and m3kern3

m3kern2: `=M3K b00A5 e00 gd1 @01 fbn00 @00 h00000000`. Windows' HAL re-enabled the GIC distributor within 0.5 s of
kernel entry (phase 0 reaches interrupt-controller init on the M3), then nothing visible from outside for 82 s: no
SPI enabled, and the sampled screen band (rows 1850..2590 of 2960 at 0xCC000000) stayed black and unchanged. The
watchdog reset as before; nothing was written. woa's EBS hook applied the same ntoskrnl patches on the M3 path as on
the A55 path (NTCFQ n=03, NTCMP n=05, NTPATCH n=08; m3early1 vs m3diag3 pmsg).

**m3kern3** (`starbite-uefi-m3kern3.bin`, sha256 `d0cc76a245a664ba21efd013ad240f5fa05f4f8f60b0af589ae0f8911e5e02c4`):
m3kern2 + the guard finds the kernel's KUSER_SHARED_DATA page by reading winload's EfiLoaderData ranges (memory
map taken at ExitBootServices into a static buffer, 2..4 GiB). The page is recognised by NtMajorVersion 10 at +0x26C
and ImageNumber 0xAA64AA64 at +0x2C. The guard then samples InterruptTime (+0x8) and TickCount (+0x320) every 0.5 s
into `=M3U uPPPPPPPP itIIIIIIII tcTTTTTTTT`. Read-only. Reading:
- u 00000000: the shared page was never initialised (or lies above 4 GiB): the kernel stopped very early in phase 0;
- it/tc growing (InterruptTime in 100 ns units, ~0x4C4B40 per 0.5 s): clock interrupts run on the M3, so the kernel
  is alive and waiting on something else (a device, a CPU, a DPC);
- it/tc frozen at a small value: the clock stopped or never ticked; the HAL timer path on the M3 is the suspect.

## 13. m3kern3 result (22:14) and m3kern4

m3kern3: `=M3K` as m3kern2 (GIC re-enabled at beat 1, then nothing); `=M3U u00000000 it00000000 tc00000000`;
`=M3E map: 0 LoaderData ranges, 0 pages`. No EfiLoaderData range lay inside 2..4 GiB, so the scan had nothing to
search. The tick question stays open. Screen (user, m3kern1/2): the logo only, no spinner.

**m3kern4** (`starbite-uefi-m3kern4.bin`, sha256 `9ca771f388e19a72058a750b27f4fcde70229a388cc2cc13c51591c909acea54`):
- the memory map at ExitBootServices is summarised per type (`=M3E mt T nCOUNT pPAGES LO-HI`, short lines), so we
  learn where winload's memory actually is;
- the scan covers EfiLoaderData and OS-defined types first, then loader code, boot-services code/data and
  conventional memory (never reserved, runtime, ACPI, MMIO), merged into up to 112 ranges;
- the guard's own map is now a 40-bit table: 0..2 GiB Device (PMU, GIC), 2..4 GiB and 34..36 GiB Normal (the two DRAM
  banks), nothing else, so the scan also reaches the high bank;
- the UFS trigger is checked every 256 scanned pages, so a long scan never delays the volume reset.
Still read-only towards Windows.

## 14. a55ctl1: the control run (healthy boot, same instruments)

`starbite-uefi-a55ctl1.bin`, sha256 `ef384254d773fc8554cf370400206bfc4fa19cbab852ecfb3bbb8f503b8f3cd4`
(`SOLO=a55ctl1 M3EARLY=3`). No hand-off: Windows boots on the A55s with woa's tables as on every normal boot. The
parked M3 core 1 runs the same guard instead of A55 0 (from ExitBootServices: own tables, the KUSER scan, the =M3K
and =M3U lines, and the reset at the UFS driver's first pmsg line). What it gives:
- whether the instruments work at all: KUSER found, `it`/`tc` growing, `gd`, `e` and `fbn` on a healthy kernel;
- the baseline timing (beats from kernel entry to the GIC enable, the first screen change and the storage driver)
  to compare with the M3 runs.
Safe: the reset comes at the storage driver's first line, before any write, as in the M3 runs.

## 15. m3kern4 result (22:45, unattended) and m3kern5 / a55ctl2

m3kern4: `=M3K` unchanged (GIC re-enabled at beat 1, then nothing); `=M3U u00000000`. The memory map at
ExitBootServices (78 entries) has **no EfiLoaderData at all**. winload's memory is EfiLoaderCode (type 1, 0x5853
pages, 0x90700000..0xBB72C000), plus BootServicesData (type 4, 0xACE3A pages, 0x90000000..0x9_0000_0000, high bank
included) and conventional memory. Scanning all of it (16 ranges) found no KUSER_SHARED_DATA signature. Either the
kernel never initialised those fields on the M3, or the signature doesn't hold this early; the control run decides.

**m3kern5** (`starbite-uefi-m3kern5.bin`, sha256 `e83c21a5c2b3eff0ec71344a1236c46516e3edb1fd429fbd69090e2f5fce02d0`) and
**a55ctl2** (control, `starbite-uefi-a55ctl2.bin`, sha256 `4407b55a1ba7411f89b7d4eaf42a26913174eb451b6d48d19ed5915d03302cb1`):
as m3kern4 / a55ctl1, plus a third line read through ntoskrnl's physical image (found at ExitBootServices: the PE at
0xBA000000 or at a LoaderCode range start, export name ntoskrnl.exe, SizeOfImage 0x103D000 = the analysed build):
`=M3N lbLLLLLLLL npNN psPPPPPPPP bcCCCCCCCC p1QQQQQQQQQQQQQQQQ`
- `lb` KeLoaderBlock (RVA D1B4B8): set in KiSystemStartup, cleared at the end of phase 1;
- `np` KeNumberProcessors (D1AB86): 1 once the boot CPU is initialised;
- `ps` PsInitialSystemProcess (D1AA20): set late in phase 0 (process manager);
- `bc`/`p1` KiBugCheckData[0]/[1] (CFE0A0): **the bugcheck code and first parameter if Windows bugchecked**.
The screen hash now covers the whole screen (every 32nd row and pixel). `=M3E ntos <base> ...` logs the values
before the kernel runs (all 0 expected).

## 16. a55ctl1 result (22:50): logs lost to the SWRESET path; a55ctl3 / m3kern6

a55ctl1: Windows on the A55s reached the storage driver and the guard's PMU SWRESET fired (back in TWRP after
~23 s, so the trigger works on a healthy kernel and nothing was written). But TWRP's /sys/fs/pstore was empty and
zone 0 read 0 bytes afterwards: that reset path loses both rings. The m3kern runs end by the loader's watchdog
and land in the menu with zone 0 intact (5/5).

**v6 reset path:** on the trigger (or the 120 s timeout) the guard marks the `=M3K` line (`=M3KU` = the UFS driver's
line fired it, `=M3KT` = timeout). It flushes both rings, then reloads the loader's own watchdog (WDT1 0x10060000,
WTCON 0xFF39, PMU WDT disable/mask bit 23 cleared, as `shim/starlte/wdt.c wdt_arm`) with count 0x10. It keeps
re-forcing that count in a loop, because Windows' UFS driver pets this watchdog as it starts. If no reset has come
after 0.2 s, PMU SWRESET follows (the volume first, logs second).
Both images are built with `KILL_SERIAL=1` (the release SerialDxe dropped, which nothing consumes) because v6 no
longer fit the release DXE slot by 0x10 bytes.

- **a55ctl3** (control, replaces a55ctl1/a55ctl2, which must not be run again): `starbite-uefi-a55ctl3.bin`,
  sha256 `8cde87265a58c6c21f8309cd03265b4d1fad2f5483187e9d9dda66299e14f389`
- **m3kern6** (= m3kern5 + v6 reset path): `starbite-uefi-m3kern6.bin`,
  sha256 `ff80e661f467de4a1033ebe0bee535a3e5370038dbff76c0974358cd7454d553`

## 17. m3kern5 result (22:54): Windows bugchecks 0x5C on the M3; m3kern7 / a55ctl4

m3kern5: `=M3N lb00000000 np01 ps00000000 bc0000005C p1000000000000020`, so **Windows bugchecked on the M3 with 0x5C
HAL_INITIALIZATION_FAILED, parameter 1 = 0x20**. KeNumberProcessors had reached 1; KeLoaderBlock read 0 (cleared by the
bugcheck path or not yet the exported copy); PsInitialSystemProcess 0 (phase 0 never reached the process manager).
The screen hash changed (`h27765CEC`, whole screen) but fbn stayed 0, so the screen didn't change after the first
sample: no BSOD drawn.

Static search: every direct KeBugCheckEx / KeBugCheck2 (0x140269860) call with code 0x5C passes an immediate
parameter 1 from {0x0, 0x104, 0x10C, 0x10E, 0x110, 0x113..0x115, 0x200..0x204, 0x300, 0x310, 0x400, 0x500, 0x2001,
0x5000, 0x5001, 0x7000, 0x8000, 0x8100, 0x8200, 0x9000} or an ASCII tag ("Its ", "Gic3"). None passes 0x20, so 0x20
is computed or the call is indirect. Parameters 2..4 should name it.

**m3kern7** (`starbite-uefi-m3kern7.bin`, sha256 `89ccfa9996a5d54ec19775dea8653f7b61760e1dfa3900c70de88108a47cc54d`)
and control **a55ctl4** (`starbite-uefi-a55ctl4.bin`, sha256 `b4d83a33002c9d58eb1e2f865b773b9113bec9a893c92626d219702faf671bdc`):
- `=M3B p2.. p3.. p4..`: KiBugCheckData[2..4];
- `=M3E pmu <cpu>: pmcr .. filt .. en ..; cycles in 1 ms N`: Windows' HAL cycle-counter setup (0x4AE830: PMCR E|LC,
  PMCCFILTR 1<<27, PMCNTENSET bit 31) replayed and measured. A55 0 does it at ReadyToBoot; M3 core 1 does it right
  before returning into winload, so on the exact CPU and state Windows inherits. Everything is restored afterwards.
  N about 0 on the M3 and non-zero on the A55 would make the cycle counter the HAL failure (Fable's PMU candidate).

## 18. m3kern7 result (23:17) and a55ctl3 (22:59): the ladder and the failing HAL step

a55ctl3 (A55s, healthy): `=M3K U0005` (storage driver at beat 5), fbn01, KUSER at PA BBC24000 with InterruptTime and
TickCount moving, `np04`, `psB2EAB080`, no bugcheck. **Every instrument works.** m3kern5/7 on the M3: `np01`, `ps0`,
bugcheck 0x5C.

m3kern7: `=M3B p2 0000000005250631 p3 1 p4 FFFFFFFFC000000?` (the last digit cut by the 60-character print).
- p2 0x05250631 is a fixed tag in two literal pools, used by exactly two 0x5C sites:
  - A, 0x14048B130 (HAL init near HalCalibratePerformanceCounter): status = the init routine `[timer+0x68]` of the
    timer object pointed to by nt+D18A18 (an indirect call), logged with a line number, p3 = the step byte nt+C0F710,
    p4 = the status;
  - B, 0x140A61114 (HAL init allocating HalQueryMaximumProcessorCount x 24 bytes).
  Both pass p1 0x110 / 0x200 as immediates, yet KiBugCheckData[1] read 0x20; to be resolved. p4 = 0xC000000x.
- PMU: on M3 core 1 at the hand-off the cycle counter was already set exactly as Windows does (winload had set it:
  PMCR 53023011, PMCCFILTR 8000000, PMCNTENSET 80000000); the counts were cut off by the print width. The HAL's own
  PMU setup at 0x4AE830 always returns success, so it isn't the failing step.

**m3kern8** (`starbite-uefi-m3kern8.bin`, sha256 `b9589ce6adfd0c81be2b578ce78a59e6fafbfa7decea04d8b2e4b2aaca8ae02d`) and
control **a55ctl5** (`starbite-uefi-a55ctl5.bin`, sha256 `c7c26417acc1171bc02304350c7709f037c4aa9bf7939815705a82587c598aa3`):
- `=M3T oOOOOOOOO fFFFFFFFF sSS stSSSSSSSS` (39 chars): the kernel's VA base from HalPrivateDispatchTable slot 1
  (file value 0x14024AF30), then o = the RVA of the timer object at nt+D18A18, f = the RVA of its init routine
  (+0x68), s = HAL step byte nt+C0F710, st = the full status;
- `=M3P <cycles per ms> cyc/ms <cpu> ...`: the number first now.

## 19. Root cause of the M3 bugcheck, and the firmware fix (m3kern9)

**p1 was cut by the print width:** `p1000000000000020` holds 15 digits after the `p1` label, so p1 = **0x200**. That's site B,
0x140A61114 (HAL phase-0 init): `KeBugCheckEx(0x5C, 0x200, 0x05250631, problem 1, status)`. The status comes from
0x14049B590, which finds the MADT ('APIC') and parses it in 0x14049B648 (problem code 1 recorded at line 0x10C).
p4 = 0xC000000D, STATUS_INVALID_PARAMETER, returned at 0x14049BA74. For every **enabled GICC**:

```
14049b978  ldr  w8, [x19, #0xc]          ; GICC flags
14049b97c  tbz  w8, #0, skip             ; disabled entries are skipped
14049b980  ldr  x0, [x19, #0x18]         ; parked address
14049b984  ldr  w9, [x19, #0x10]         ; parking protocol version
14049b988  ldr  w8, [nt+D168E0]          ; "start processors with PSCI" (HalStartDynamicProcessor needs it)
14049b98c  tbnz w8, #0, ok
14049b990  cmp  w9, #1 ; ccmp x0, #0, #4, eq
14049b998  b.eq -> C000000D               ; no PSCI: version must be 1 AND the mailbox non-zero
```

On A55 0 the woa EL2 shim answers PSCI_VERSION 0x10000, the PSCI flag is set, and the boot GICC with parking v0 and no
mailbox is accepted. On the M3, Samsung's EL3 answers FFFFFFFF (m3diag1), so the flag stays clear and **every** enabled
GICC, the boot CPU's included, must use ACPI parking protocol v1 with a mailbox. Our commit (and the whole old solo
track's MADT) gave M3 core 1 parking v0 / address 0.

**Fix (ACPI tables only, standards-conformant):** M3 core 1's GICC carries parking protocol version 1 and its own
reserved, zeroed 4 KiB mailbox page, also in its DSDT `_MAT` copy. This is the ACPI parking protocol (ACPI 6.x 5.2.12.14)
that a platform without PSCI is expected to use. It is also the path for adding M3 2, 3, 0 later (parked in our
stubs, mailboxes in the MADT).

**m3kern9** (`starbite-uefi-m3kern9.bin`, sha256 `7f4f05e95dc6bfe074f8d45123b891b0b6baa6b4c38848f4eed0409c810a00e8`):
m3kern8 + this fix. Same guard: if Windows now gets as far as its storage driver, A55 0 resets through the watchdog
before any disk write (`=M3KU`).

## 20. m3kern9 result (23:34): Windows' kernel runs on M3 core 1; m3run1 (free run)

m3kern9: `=M3K U0002 ... fbn01 @02` (the storage driver at beat 2 = 1 s, the spinner animated), `=M3U uBB725000
it0070BEDA tc0000002F` (the clock ticks on the M3), `=M3N np01 psBD2AE080 bc0` (1 CPU, process manager, no bugcheck),
`=M3T s16 st0` (timer init as on the A55), `=M3P 1066174 cyc/ms` on M3 1. **The parking v1 mailbox was the fix.** The
guard stopped Windows at its storage driver as designed (nothing written).

**m3run1** (`starbite-uefi-m3run1.bin`, sha256 `2512f52ad407cdf9d4bdf81bf05aa25036032c5aca94dbb1824d665b846c5e0b`,
`SOLO=m3run1 M3EARLY=4`): m3kern9 with the volume guard turned into a **free run**. A55 0 never resets; it keeps
sampling the =M3K/=M3U/=M3N/=M3B/=M3T lines and is the **liveness watchdog** outside Windows: it pets the loader's
WDT1 (WTCNT = WTDAT) only while Windows' InterruptTime advances (or during the first 60 s before KUSER is found). A
Windows that stops ticking is reset by the loader watchdog within 82 s; a live one never is. Windows runs on M3
core 1 alone (1 CPU). The image is a SOLO build (no SBGPU/USB/WLAN blobs): Basic Display, no USB link.

How it should end: a clean restart from inside Windows. The existing one-shot autotest
(`tools/win/autotest-cycle.sh OUTDIR --key win11-m3probe --cpu build/win/sbcpu`, which arms C:\starbite\auto\enabled;
Windows restarts itself after the test, dead man `shutdown /r /t 600`) gives a clean restart and `cpu.log` (MIDR
531F0020 = Mongoose, 1 CPU). A restart of Windows on the M3 goes through the FADT reset register / UEFI
ResetSystem, which reset the whole SoC (A55 0 included).

## 21. m3all3: M3 core 1 boots, M3 cores 2 and 3 join as parked secondaries (3 big cores)

Windows' HAL starts secondaries with the ACPI parking protocol when there is no PSCI: 0x140491F80 copies its AP
startup code into the mailbox's OS half (+0x20, 0x6B0 bytes), writes JumpAddress (+8), then CpuId (+0) = what
Cores8's parking stub (firmware half, +0x800) waits for: JumpAddress != 0 and CpuId == its UID, acknowledge, jump with
x0 = the mailbox (MMU off, EL1). C2 kept parked M3s alive in this stub through a whole Windows session.

**m3all3** (`starbite-uefi-m3all3.bin`, sha256 `695dda08955441c2eb3b6cf2ab4ce3377e832ac9e9682c88cee6d14d84efc5cf`,
`SOLO=m3all3 M3EARLY=5 KILL_SERIAL=1`): m3run1 (free run, A55 0 as the outside liveness watchdog) plus, at
dispatch, M3 cores 2 and 3 CPU_ON'd (PMU defaults first) into the parking stub in a reserved mailbox page each.
The committed MADT enables UID 5 (boot, own mailbox), 6 and 7 (parking v1, their mailboxes), with A55s and M3 0
disabled; DSDT `_MAT` synced. `=M3S core n rc .. mpidr .. mbox ..` lines show each park; `=M3E commit: ... on: 4n 6Y 7Y`.
Expect `=M3N np03` and Task Manager / cpu.log with 3 CPUs, all MIDR 531F0020. M3 core 0 follows (its first CPU_ON
needs the FlexPMU core-on steps Starbite OS uses). To make room in the release DXE slot, free-run builds compile out
the PMU replay and the ACPI table lines (their results are known from the m3kern runs).

## 22. m3all4: all four M3 cores (ATTENDED ONLY)

`starbite-uefi-m3all4.bin`, sha256 `351d738f29769c256c57766a984d12ae08f9ab6438bb8f93848b82f843112c15`
(`SOLO=m3all4 M3EARLY=6 KILL_SERIAL=1`): m3all3 plus M3 core 0 (target 0x100, MADT UID 4) parked last. Before its
CPU_ON come Samsung's FlexPMU core-on steps that Starbite OS applies before every M3 start (libsb `m3_fp_on`):
CPU_INFORM(4) = 0, bit 0 cleared in GRP2_INTR_BID_ENABLE (0x14070200) and PMU_EVENT_INTERRUPT_ENABLE_GRP2
(0x14067F08). The cluster is already on, so the APM2AP mailbox bit stays. Starbite OS starts 0x100 this way
reliably. In the UEFI context this core's first CPU_ON is still new, and woa saw it take the SoC down without the
PMU steps, so **only with the user present** (a hang there needs a forced reset; it is before Windows, so safe for
the volume). Expect `=M3S core 0 rc 0 ...`, `on: 4Y 6Y 7Y`, `=M3N np04`, cpu.log 4 CPUs.
The MADT then lists M3 0 (MPIDR 0x100) and the A55s stay disabled, so the 0x100 affinity collision with A55 core 1
doesn't reach Windows.

## 23. m3run1 result (23:46): Windows 11 runs on an Exynos M3 big core

Booted 23:46:19 on M3 core 1 alone. It reached logon (sb-autotest.cmd ran at logon and restarted Windows by itself) and
was back at the menu after 588 s. The registry hive is clean (179290/179290). zone 0: `=M3K b0444 e03 gd1 @01 fbn4B`
(about 546 s alive, SPIs enabled by drivers, the screen changed 75 times), `=M3U it4543AAEF tc0000886C` (ticking),
`=M3N np01 ps28A7A080 bc0`. Our Wi-Fi driver brought up the BCM4361 and sblinkd ran: drivers work on the M3. No cpu.log
(the phone's autotest.ps1 is an older version), so there is no MIDR from inside Windows yet: `tools/woa/win-m3/sb-cpuproof.cmd`
(one-shot Startup item, deletes itself) writes C:\starbite\auto\cpuproof.txt with NUMBER_OF_PROCESSORS,
PROCESSOR_IDENTIFIER, the CentralProcessor registry keys (`CP 4000` = MIDR_EL1) and sbcpu-arm64's output.

## 24. m3all3 result (00:0x): Windows 11 on three M3 cores; m3all3c: 1.8 GHz with a thermal guard

m3all3: `=M3S core 2/3 rc 0 ... 1 ms`, `on: 4n 6Y 7Y`, `=M3N np03`; cpu.log: 3 logical processors, CentralProcessor
0/1/2 `CP 4000` = 0x531F0020 (all M3), no PF 34 (LSE correctly not advertised), caches L1 64K/64K, L2 512K, L3 4M,
20M mul-add 101/112/121 ms per core at 1066 MHz. Open: the clock; c000001d in winsqlite3.dll 3.51.1.0 (+0xEC614,
services WpnUserService/DiagTrack/OneSyncSvc; M3-specific, binary still to be pulled); xtajit64 c0000409
(fail-fast, the known CNTFRQ_EL0 = 0 problem of the x64 emulator, A55 path too).

**m3all3c** (`starbite-uefi-m3all3c.bin`, sha256 `f6b9332fac5e00bdf5520c1fe156110b26ed8cd0b70f6a5d723e0561ca8ebe7d`,
`SOLO=m3all3c M3EARLY=5 M3KHZ=1794000 KILL_SERIAL=1`): m3all3 plus
- right before winload resumes (on M3 core 1, after the table commit), one ACPM DVFS FREQ_REQ {domain 3 = CPUCL1,
  1794000 kHz} on channel 0 of the AP channel table, libsb `acpm.c` format, reply matched by its sequence number:
  `=M3C 1794000 kHz M3 cluster: answered`;
- A55 0's guard reads the M3 probes (TOP TMU 1, 2, 3, 4, 6; libsb `tmu.c` calibration) every 0.5 s:
  `=M3H tTT hiHH sS` (hex degrees C now / highest; state 0 ok, 1 throttled, 2 reset). At >= 85 C it sends one
  FREQ_REQ to 1066 MHz (fire and forget, same channel) and stays there. At >= 100 C it resets through the loader
  watchdog (21f). No governor yet.
- ACPM sharing: the Windows-side ACPM user is sbwddm (GPU, domain 4 only; it isn't loaded by these SOLO images).
  The guard and that driver would share channel 0's queue, so a combined image needs one owner.
Free-run builds no longer carry the =M3B/=M3T/watch lines, the framebuffer hash or the PMU replay (room in the DXE slot).

## 25. winsqlite3.dll 3.51.1.0 needs ARMv8.1 LSE and ARMv8.3 RCpc unconditionally

`build/woa/cores/m3all3-run1/winsqlite3.dll` (pulled read-only from the phone): at +0xEC614 `casal w8, w19, [x9]`,
inline in an ordinary function, with no CPU-feature check. In the whole image: 8 CASAL, 10 CASP, 2 SWPAL, and
**92 LDAPR/LDAPRH** (ARMv8.3 RCpc). The Exynos M3 is ARMv8.0 (ISAR0 0x11120 = no atomics, ISAR1 0 = no LRCPC), so every
process that reaches these paths dies with c000001d (WpnUserService, DiagTrack, OneSyncSvc on m3all3). This
Microsoft build is compiled for a newer architecture level than 23H2's baseline.

- Firmware can't fix it: an undefined instruction at EL0 is taken to EL1 (Windows). Trapping it would need EL2 with
  HCR_EL2.TGE, which breaks EL1, and Samsung's EL3 keeps EL2 from us on the M3 (HVC undefined, m3diag1).
- Windows-side options are the user's decision: leave those services failing (they are non-essential: push
  notifications, telemetry, mail/calendar sync), or disable them. Replacing the DLL would modify a Windows file
  (excluded).
- More Windows components may be built this way over time. `tools/woa/win-m3/lse-scan.py FILE_OR_DIR...` counts LSE
  (CAS/CASP/LD<op>/SWP) and LDAPR encodings in ARM64 PE code sections. ntoskrnl, ntdll and kernelbase contain LSE
  too, but only in their checked outline-atomics helpers (they run fine on the M3). A large LDAPR count, like
  winsqlite3's 92, is the red flag. Run it on a read-only copy of C:\Windows\System32 to list the candidates.

## 26. m3all3c result: the clock request faulted on the M3; m3all3d moves it into the guard

m3all3c hung before Windows (loader watchdog; hive clean). Up to the commit everything matched m3all3, but no `=M3C`
line, and the guard never beat (`=M3K b0000`). The cause: the C request ran on M3 core 1 through **UEFI's page tables**,
which map only a fixed list of MMIO windows (PlatformMemoryMapLib: framebuffer, rings, a few named blocks). APM SRAM
(0x02039000) isn't one of them, so the M3 took a data abort at the first SRAM read, before prog 7. The guard,
which starts at prog 7, never ran.

**m3all3d** (`starbite-uefi-m3all3d.bin`, sha256 `dda69fd24e6cc64ff0b0fa2349fdbf757f3b9e70c696bf9bf18e922f477c436d`):
the request is sent once by the **guard** at its entry, through its own page table (0..1 GiB Device: APM SRAM, the
ACPM mailbox, the TMU and the watchdog are all reachable), fire and forget (no wait). The kHz value is a guard literal
(+0x2C). Crumb `c` in `=M3H t.. hi.. s. cN`: 1 written (and cleaned to RAM) before the attempt, 2 after. A hang
inside ACPM shows as c1. Thermal guard as in m3all3c.

## 27. m3all3d result: 3 M3s at 1794 MHz (2.2x); windows-big3-1794: the user's normal firmware on the big cores

m3all3d: Win32_Processor 3 cores at 1794 MHz, 20M mul-add 45.9 / 45.4 / 55.5 ms (2.2x m3all3), `=M3H t33 hi46 s0 c2`
(51 C, peak 70 C, never throttled), clean restart. sbcpu-x64/x86 c0000409 (emulator, CNTFRQ), sbcpu-arm64 fine.

**windows-big3-1794.uefi** (sha256 `a0bb8a2679b269f3342a17c767ae36524d16749f1a85022f1429b645899911ba`) = the `win11` row's
firmware `windows-t32s-nocache.uefi` (c3e43fd2: USB device, SBGPU, WLAN4361, SMBIOS) with only Star2LteSsdtDxe
replaced by m3all3d's module (same blob installer, plus M3 core 1 boot, M3 2/3 parked, parking-v1 MADT, 1794 MHz,
liveness + thermal guard). The old module is blanked (`--kill-blank`). Everything else is byte-identical: bootshim,
SEC/PEI file, every other DXE module and blob.

The release DXE volume no longer fit its 0x51000 outer FV (0x1848 over). That size was only GenFv's auto-size
(FVMAIN_COMPACT `NumBlocks = 0`); SEC/PEI were built with PcdFvSize = PcdFdSize = 0x700000 and walk the volume by its
header. `FV_GROW=1` (fv-splice.py) lets the header claim more of the FD: here 0x51000 -> 0x53000 (FvLength, block count,
header checksum; 4 header bytes change, the tail stays 0xFF). This is new: if this image stops before the Windows
logo, the grown outer FV is the suspect (the loader watchdog brings the phone back; Windows never started).

Windows-side on the M3s: our own drivers (sbwddm, sbwdog, sbkeys, sbwificx) have no unchecked LSE (lse-scan).
sbwddm uses ACPM channel 0 for G3D. The guard sends on that channel only at hand-off (before Windows' drivers) and
once more if it throttles at 85 C: a rare overlap window, acceptable until there is a single ACPM owner.

## 28. windows-big3-1794 result (01:03): the user's firmware on 3 M3s, GPU driver in full mode; x86/x64 emulation

windows-big3-1794: desktop, 3 x 531F0020 at 1794 MHz ('Samsung Exynos 9810' via SMBIOS), sbwddm WddmMode 1 (full),
Wi-Fi and link up, 73 C peak, clean restart, hive clean.

**x86/x64 emulation on the M3 (not the CNTFRQ problem).** On the A55s the E1 setting (ScaleRdtsc = 0, SOFTWARE hive)
already makes sbcpu-x64/x86 exit 0 (results-e1, results-auto7). On the M3 every emulated process fast-fails with
c0000409 at xtajit64 +0x36D6C (sbcpu-x64, sbcpu-x86, SysWOW64 cmd.exe, steam.exe) before printing anything.
- +0x36D6C is the emulator raising the *guest's* fail-fast (its "INT n" trap BRK #0xF012 with an invalid vector,
  reached from its STATUS_ILLEGAL_INSTRUCTION handler, which matches the faulting word against its own trap markers
  BRK #0xF00E..#0xF013). So an illegal instruction happened inside emulation on the M3 and was not one of the
  emulator's markers.
- xtajit64 reads PF 34 (LSE) at start-up (+0x18238, +0x309F4) and keeps the result, yet the binaries carry CAS/CASP/LD<op>
  (lse-scan: xtajit64 53, xtajit 38). Likeliest: a code path (generated or runtime) still uses an ARMv8.1+ instruction
  on a host without it. Microsoft never supported Windows 11 x64 emulation on ARMv8.0 SoCs (Snapdragon 835).
- Firmware cannot add instructions or catch EL0 undefined instructions (section 25). Practical answer: x86/x64 apps on
  the A55 Windows row, native ARM64 apps on the big-core row. To confirm the instruction: the WER reports
  (C:\ProgramData\Microsoft\Windows\WER\ReportArchive\AppCrash_sbcpu-x64*\Report.wer) name the exception address.

**Governor (c), proposed:** a thermal-step governor in the A55 guard, outside Windows: from the TMU, step the M3 cluster
up through the M3 table (1794 -> 2002 -> 2314 MHz) while the hottest probe stays under ~70 C, and down above ~80 C,
with hysteresis and at most one ACPM request per few seconds. It needs one ACPM owner: sbwddm (GPU session) sends on
the same channel-0 queue, so either the guard owns ACPM and sbwddm asks it (shared word), or sbwddm gains a domain-3
policy. Load-driven scaling (from Windows' idle time) would need a Windows-side driver.

## 29. ACPM arbiter and the temperature step governor (guard v15): m3all3g, windows-big3g

Decision (main session): on big-core boots the A55 guard owns the ACPM DVFS queue; sbwddm posts its GPU requests to
the guard. Interface: side-session-notes `acpm-arbiter.md` (sent to the GPU session): page 0xFED1F000 ('SBAR', v1,
heartbeat every 0.5 s, one request slot req_seq/domain/kHz, ack_seq/status, allowed-domain mask 0x10 = G3D, M3 kHz and C).
sbwddm detects it by magic + a moving heartbeat (the page survives warm resets); else direct mode.

Guard v15 (free-run builds only):
- the code moved to its own page (literal page, code page, table page; 6 reserved pages), 0x9B8 bytes now;
- the ACPM routine first drops stale replies (libsb acpm_send), so fire-and-forget requests never fill the reply
  queue, and returns 1 sent / 0 queue full;
- arbiter: page initialised at guard entry (magic written last), heartbeat and M3 kHz/C each beat, the request slot
  polled every loop iteration and forwarded (status 1/2/3);
- governor each beat: steps 1794 / 2002 / 2314 MHz (index in `=M3H ... gN`), up below 70 C, down above 80 C, at most one
  change per 3 s; 85 C throttle to 1066 MHz (s1) and back to 1794 below 70 C; 100 C watchdog reset (s2).

Images: `starbite-uefi-m3all3g.bin` (SOLO, 0f038088...) and **`windows-big3g.uefi`** (e5000b31...: the win11 row's
firmware with only Star2LteSsdtDxe swapped, outer FV grown as big3-1794).

**ACPM ownership closed (01:3x):** sbwddm never used the DVFS queue (PMIC channel 2 only). The channel-0 writer was
sbthermal's A55 governor; GPU session T34 (sbthermal db7ef111) stands it down on big-core boots (non-A55 CPU or a live
arbiter). With T34 installed the guard is the only channel-0 writer on big-core boots.

## 30. big3g result (01:22) and guard v16 (conservative governor): windows-big3h

big3g: `=M3H t4B hi56 s0 c2 g2`: it reached 2314 MHz, the peak was 86 C and the final 75 C. `s` shows only the current
state: a throttle at >= 85 C goes back to s0 after cooling below 70 C (by design), so the line couldn't tell whether
it fired. The 0.5 s beat also let the temperature overshoot the 85 C threshold.

**v16:** throttle at 80 C (to 1066 MHz), down-step above 75 C, up-step and throttle recovery only below 65 C, and a
throttle-event counter `nN` on the line (`=M3H tTT hiHH sS cC gG nN`, saturating at F). The throttle already takes the
hottest of all five M3 probes (TOP TMU 1, 2, 3, 4, 6) every beat. `windows-big3h.uefi` (7e14e11e...),
SOLO `starbite-uefi-m3all3h.bin` (0b208195...).

WER (big3-run1/wer): sbcpu-x64 on the M3 = fast-fail code 5 raised by xtajit64 at +0x36D6C (no guest address in the
report); on the A55 before E1 it was c0000094 at +0x30C70 (the CNTFRQ divide). To see the guest instruction a minidump
is needed (WER LocalDumps for sbcpu-x64.exe: a Windows configuration change, the user's call).

## 31. big3h result (01:44) and v17 (governor capped at 2002 MHz): windows-big3i

big3h: `=M3H t41 hi55 s0 c2 g1 n5`: peak 85 C, five throttles, ended at 2002 MHz; mul-add 63 / 81 / 49 ms. sbthermal T34
reported `Acpm 'sbthermal: big-core boot, the guard owns ACPM', BigCore 3, Midr 0x531f0020`: the arbiter handshake
works. 2314 MHz heats the cluster faster than a 0.5 s beat can follow.

**v17:** the governor tops out at 2002 MHz (step 1) and changes at most once per 10 s (20 beats); thresholds as v16
(throttle 80 C, down > 75 C, up < 65 C). `windows-big3i.uefi` (dca74622...), SOLO `starbite-uefi-m3all3i.bin` (b6a726f7...).
Fixed 1794 MHz peaked at 73 C (big3-1794), so 2002 MHz should stay below the 80 C throttle under the same autotest load.
If big3i shows n0 and hi < 0x50, it can replace big3-1794 as the daily image (+12% clock).

## 32. big3i passes daily-use criteria (02:09)

`=M3H t3C hi4E s0 c2 g1 n0`: peak 78 C, no throttle, ended at 2002 MHz; np03, clean restart, hive clean. The main session
makes `windows-big3i.uefi` a visible catalog row "Windows 11 (big cores)" beside the unchanged win11 row. User-facing
status: side-session-notes `win-m3-clean-user-status.md`. Known limits on that row: x86/x64 emulation (section 28) and
the winsqlite3 services (section 25). Next: m3all4 (attended), then a big4i variant.

## 33. x86/x64 on the big-core row: host groundwork for T3 and T6 (docs/WIN11-M3-X86.md)

Host only, nothing booted. Details and the pull list: notes folder `win-m3-clean-x86-prep.md`.
- T3: `tools/woa/win-m3/hive-localdumps.py` (offline SOFTWARE hive: WER LocalDumps for sbcpu-x64.exe / sbcpu-x86.exe,
  DumpType 2, DumpFolder C:\sbdumps REG_EXPAND_SZ, DumpCount 2; show/set/remove; hive-service.py safety). Rehearsed
  set and remove on pull-1111's SOFTWARE: remove restores "no LocalDumps key".
- T3: `tools/woa/win-m3/minidump-m3.py` (exception, the xtajit64 +0x36D6C / xtajit +0x5EF68 thread, x19 record,
  x25 CONTEXT.Pc at +0x108, [x29+0x14], region = module+RVA / private exec JIT / mapped exec, word class via m3-scan).
  Checked on a synthetic dump only.
- XtaCache: no .jc contents on the host (listing only). 11 files (~1.7 MB) to pull: the four SBCPU caches, x64 and
  x86 KERNEL32/KERNELBASE, x86 NTDLL, SysWOW64 CMD. There is no x64 NTDLL .jc (x64 system DLLs run as ARM64X/EC).
- T6: prebuilt llvm-mingw 20261104 aarch64 includes arm64ec, so libarm64ecfex.dll builds in the lima VM without an
  LLVM build. Patches A/B/C: `tools/woa/win-m3/fex/0001-starbite-23h2-m3.patch` (applies to e3b30dd; not compiled);
  script `tools/woa/win-m3/fex/build-fex-arm64ec.sh`. Not run: the Mac has 4.5 GiB free; the script needs >= 6.
- Windows Update fails on the big-core row (same winsqlite3 crash); noted in the user status note.

## 34. sbxta.sys: XtaCache per CPU class (after T2 passed)

T2 (main session, x86t2-2): with the XtaCache service Start=4 and C:\Windows\XtaCache renamed, sbcpu-x64, x86 and
x64fresh exit 0 on the big-core row and SysWOW64 cmd runs. The persistent cache made on the A55 (LSE, LDAPR) is what
broke emulation on the M3. Permanent switch: `starbite/win/sbxta/sbxta.c`, a WDM system-start driver (Start 1, before
the SCM). It reads MIDR_EL1: M3 -> `Services\XtaCache\Start` = 4; A55 -> back to 2 if it is 4; other -> nothing. It
logs to `Services\sbxta\State`. An optional folder swap (`Parameters\SwapCache` = 1: XtaCache <-> XtaCache.a55 /
XtaCache.m3, keyed on XtaCache.a55 existing) is for the case T2b shows the folder must also go; it is off by default.
Staged build/woa/cores/sbxta-1/sbxta.sys 81ea774f..., build 42056cc, test-signed; install line and two-boot test in
the notes folder `win-m3-clean-sbxta-request.md`. Not yet run on the phone.

T2b (main session, x86t2b-1): XtaCache Start=4 with the A55 cache folder left in place also passes (sbcpu arm64/x64/
x86/x64fresh exit 0, SysWOW64 cmd, Steam client + steamwebhelper ran 150 s with a window, no Steam crash events).
The service switch alone is enough: sbxta with SwapCache 0 (default).

## 35. sbxta-run1 passed: x86/x64 fixed on the big-core row

Main session, `build/woa/cores/sbxta-run1/` (log `sbxta-run1.log`), sbxta-1 installed with Start 1:
- A55 boot (win11): State Boots 1, Midr 0x410FD051, Cpu A55, XtaCache Start 2 -> 2, status 0. sbcpu arm64/x64/x86/
  x64fresh exit 0, SysWOW64 cmd 7, 0 application errors.
- M3 boot (win11-big): Boots 2, Midr 0x531F0020, Cpu M3, Start 2 -> 4, status 0, Result `sbxta 42056cc: M3 MIDR
  531F0020, XtaCache Start 2 -> 4 (0x00000000), swap: off`. All four sbcpu exit 0, cmd 7; only the 10 known winsqlite3
  events.

sbxta stays installed (pre-install SYSTEM backup `backup/*-pre-sbxta`). This is the x86/x64 fix for the big-core row;
docs/WIN11-M3-X86.md section 5 has the summary. The 4 -> 2 restore on the next A55 boot is checked by the main session
after its T38 cycles. Still open on the big-core row: the winsqlite3 services (section 25) and the 4th M3.
