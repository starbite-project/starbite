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
