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
