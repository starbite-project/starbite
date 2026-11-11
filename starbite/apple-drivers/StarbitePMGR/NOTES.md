# StarbitePMGR: notes

CPU power manager for XNU's `USE_APPLEARMSMP` path: brings up 8 CPUs in two clusters. Milestone M2
(docs/APPLE-S9-DRIVERS.md 2.3, Part 3 rows 4/5/27, Part 5). Status 2026-11-11: written, host-tested,
compile-checked; nothing has run on the phone.

## 1. Facts and sources

| Fact | Value | Source |
|---|---|---|
| Cores | logical 0-3 Cortex-A55 (cluster 0, 'E'), 4-7 Exynos M3 (cluster 1, 'P') | tools/board/starlte.board |
| A55 MPIDR | 0x81000n00 (MT set, core in Aff1); ADT reg n<<8; PSCI target n | starsmpprobe=1 on the phone (starlte_smp.c), SweetCPU.cpp |
| M3 MPIDR | 0x8000010n (no MT); ADT reg 0x10100+n; PSCI target 0x100+n | phone, patch 0304 |
| Bit-16 quirk | M3 Aff1:Aff0 (1:n) collides with A55 core 1 (0x100): start.s reset_vector and ml_get_cpu_number add bit 16 when MPIDR.MT = 0 | osfmk/arm64/start.s 148-154, machine_routines.c ml_get_cpu_number, tools/mkadt_starlte.py |
| PSCI | 0.1 only; PSCI_VERSION answers -1; CPU_ON 0xc4000003, CPU_OFF 0x84000002, CPU_SUSPEND 0xc4000001 | dts psci, ADT /psci, starlte_smp.c probe |
| Conduit | SMC #0. HVC is UNDEFINED at EL1 (Samsung uH owns EL2) | docs/APPLE-S9-DRIVERS.md 2.1 |
| CPU_ON entry | `sweet_secondary_entry` (start.s): physical, MMU off, x0 ignored, EL1 or EL2 handled, finds its cpu_data by MPIDR | osfmk/arm64/start.s |
| M3 PMU CAL | PMU 0x14060000: +0x1088 [31:24] = 0x8d (CPU_QCH_CLUSTER1_NONCPU_SYS_PWR_REG); +0x2114/0x2180/0x21ec/0x2258 [1:0] = 2 (MEMORY_CLUSTER1_CPU0..3_OPTION); +0x2384 [1:0] = 2 (_NONCPU_OPTION). Without them CPU_ON returns 0 but the M3 never runs | patch 0304 (SweetCPU.cpp `starlte_m3_pmu_defaults`), from the Samsung 4.9 pmucal_lpm_init via the Linux S9+ port |
| MIDR | A55: implementer 0x41 part 0xd05; M3: implementer 0x53 | sleh.c (the M3 detection the LSE/A55 pinning uses) |
| Timebase | 26 MHz; CNTFRQ_EL0 reads 0 (set only at EL3), XNU takes it from the ADT | starlte.board, Part 2.7 |
| Idle | WFI, executed by XNU's `cpu_idle()` itself; no deeper state | osfmk/arm64/cpu.c cpu_idle |

## 2. The bring-up, step by step (AppleARMSMP.cpp, public)

1. `IOCPUInitialize` -> `cpu_boot_thread`: waits for the platform expert, makes the `PassthruInterruptController`,
   waits for its child (StarbiteGICv2), waits for an `IOPMGR` (this driver).
2. For each selected CPU: `initCPUIdle` (we set `supports_nap` false), `ml_processor_register`,
   `register_aic_handlers` (waits for the cpu nub, needs its 3 interrupts so `aic_ipis` is set: see
   ../StarbiteGICv2/NOTES.md 2).
3. `processor_boot(boot CPU)`: `PE_cpu_machine_init` broadcasts CPU_BOOTED (our callbacks: GIC bit, identity
   check), sends the self-IPI, waits for it.
4. `processor_boot(each secondary)` -> `cpu_start` -> `PE_cpu_start_internal` -> `enableCPUCore(cpu, 0)`:
   hw `sb_smp_cpu_on`: descriptor check -> (M3) PMU CAL written and read back -> state ON_SENT -> CPU_ON.
   ON_SENT is set before the SMC because the core can run and report in before the SMC returns (the host
   simulation caught this race).
5. The secondary: `sweet_secondary_entry` -> reset_vector -> `start_cpu` -> `arm_init_cpu` -> `pe_init_fiq`
   (pexpert GIC per-CPU init) -> `PE_cpu_machine_init`: CPU_BOOTED on itself (GIC bit + handler; PMGR checks
   MPIDR/MIDR) -> self-IPI -> `processor_cpu_reinit` waits for it (`wait_for_cpu_signal = true` under
   USE_APPLEARMSMP).
6. `ml_cpu_init_completed`, `IOAllCPUInitialized` published. Our self-test thread then: waits up to 5 s for
   every started core to check in; `cpu_xcall` from CPU 0 to each other core (retried while SIGPdisabled);
   a 20 ms `IOSleep` bound to each core (its PPI 27 must fire); prints the per-CPU lines and
   `StarbitePMGR: M2 PASS|FAIL: cluster E: n up, cluster P: n up, IPI ok, timer 26 MHz`; then
   `sbstop=smp` resets into TWRP (0423).

A core that fails (PMU read-back, PSCI error, identity) is logged and left off; no panic, so the self-test
can say exactly which one ("cluster P: 3 up of 4"). XNU never schedules on a core that did not check in.

## 3. Kernel config (proposed, not applied)

- `board-config-two-clusters.diff` (pexpert headers): STARLTE gets `MAX_CPU_CLUSTERS 2`, `__ARM_AMP__ 1`,
  `USE_APPLEARMSMP 1`; `NO_ECORE` is no longer defined for STARLTE (unused in this tree, but wrong).
  `MAX_CPU_CLUSTER_PHY_ID` defaults to 1. Without `HAS_CLUSTER` (Apple-only) `ml_parse_cpu_topology` numbers the
  clusters from cluster-type ('E' -> 0, 'P' -> 1), so the ADT must say cluster-type 'E'/'P' (`--topology amp`).
  Compared with VMAPPLE.h: same `USE_APPLEARMSMP`; unlike it, 2 clusters + AMP, GICv2, no PAC, no
  APPLEVIRTUALPLATFORM (so XNU passes entry 0 to enableCPUCore and we supply the entry ourselves).
- `kernel-two-clusters.diff`: what the switch breaks in the build, found by syntax-checking 446 kernel sources
  with the patched headers:
  - `osfmk/kern/sched_clutch.c`: the Edge scheduler's stir-the-pot registry is a 128-bit atomic that needs ARMv8.1
    CAS (`#error` under `OS_ATOMIC_USE_LLSC`). Our kernel is built for armv8-a (the M3 has no LSE). With
    MAX_CPUS 8 the registry needs 16 bits: use a 64-bit registry when LL/SC and 2*MAX_CPUS <= 64.
  - `osfmk/arm/cpu_common.c` `cpu_exit_wait`: under USE_APPLEARMSMP it polls Apple's CPU PIO status register
    (`CPU_PIO_CPU_STS_OFFSET`, undefined here). Skipped for STARLTE (CPU exit is not offered: `ml_cpu_can_exit`
    is false without a boot-arg).
  With both, all 446 files compile (osfmk/kern, osfmk/arm*, osfmk/vm, osfmk/kperf, iokit/Kernel, bsd/kern,
  pexpert). Not linked or booted.
- `integration.diff`: KernelConfigTables (StarbiteGICv2 + StarbitePMGR personalities, SweetCPU dropped for
  STARLTE), SweetCPU.cpp compiled out under USE_APPLEARMSMP, SweetPlatformExpert publishes the gic nub.
- `adt-m2.diff`: gic `#interrupt-cells = 1`; cpu nodes `interrupts <1 pmu 1>` + `interrupt-parent` (amp only);
  `pmu_spi` per core in starlte.board.

## 4. Kernel symbols used that are not public KPI

Built in: fine. A Boot KC build needs exports for: `sweet_secondary_entry`, `thread_bind`, `cpu_to_processor`
(self-test only), `cpu_xcall` (Private), `ml_get_topology_info` (Private), `ml_static_vtop` (Private),
`cpu_event_register_callback` (Private), `starlte_sbstop_at/_now` (0423, test only), `gPEClockFrequencyInfo`.

## 5. Open questions

- **Q1. CPU_ON with an entry above 4 GiB.** The S7's firmware kept only 32 bits (patch 0223: CPU_ON returned 0,
  the core never arrived). The S9's is unmeasured: today's 12377 kernel runs at 0x92000000, but the Apple chain
  loader's M1 hand-off put the kernel at 0x88xxxxxxx (build/apple/m1b-run1). If the first cpus=4 run shows
  "CPU_ON ok but NEVER CHECKED IN" for every secondary, this is the first suspect. The driver supports a
  trampoline: give the /psci node `starbite,entry-trampoline` = a free, 16-byte-aligned PA below 4 GiB (which
  page is free in the M2 memory map is for the loader side to say; 0x91ffe000/0x91fff000 are taken).
- **Q2. Edge scheduler without CLPC.** Apple's AMP machines run Edge with CLPC's recommendations; we have none
  (no AppleCLPC). Edge's defaults recommend every core; early boot on Apple hardware runs the same way before the
  CLPC kext loads. Unverified on our port; M2 only proves the CPUs come up and are schedulable.
- **Q3. The HEROLTE scheduler hacks** (0238 `sweetfast`, RT fast-core picks) still compile and are no-ops inside
  a per-cluster pset, but should be guarded with `!__AMP__` before M3; the LSE pinning in sleh.c (thread_bind to
  an A55) keeps working.
- **Q4. CPU_OFF / sleep.** `disableCPUCore` panics with a clear message; nothing calls it unless CPU exit is
  enabled by boot-arg or the system sleeps. PSCI CPU_OFF (0x84000002) is in the ADT for later.
- **Q5. PMI.** The cpu nodes name each core's PMU SPI, but XNU registers a PMI handler only with CPMU_AIC_PMI
  (Apple). Harmless; listed so nobody expects perf interrupts.
