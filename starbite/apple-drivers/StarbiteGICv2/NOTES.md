# StarbiteGICv2: notes

Driver for the S9's GIC-400 (GICv2) under XNU's `USE_APPLEARMSMP` path. Milestone M2 (docs/APPLE-S9-DRIVERS.md
Part 3 row 3, Part 5). Status 2026-11-11: written, host-tested, compile-checked; nothing has run on the phone.

## 1. Register facts and sources

| Fact | Value | Source |
|---|---|---|
| GICD | 0x10101000, 0x1000 | tools/board/starlte.board `gic`; dts `interrupt-controller@10100000` |
| GICC | 0x10102000 (0x2000 in the board file, 0x1000 in the base ADT; we use < 0x100) | same |
| Product | GIC-400, GICv2 with Security Extensions; we are the non-secure side | ARM IHI 0048B; dts `arm,cortex-a15-gic` |
| Group 0 | TrustZone's. GICD_IGROUPR is RAZ/WI to us; non-secure writes to a Group 0 INTID's registers are ignored | pe_fiq.c comment (A5XELTE branch), GICv2 4.3.4 |
| Timer | virtual timer PPI 11 = INTID 27, level, all CPUs | dts `timer {}`, ADT `virtual-timer-intid`, A5XELTE_VTIMER_INTID |
| IPI | SGI 1 for every XNU signal (pexpert enables it per CPU) | pe_fiq.c, patch 0114 |
| SGI source | GICC_IAR[12:10] = the sender's CPU interface number; sleh masks INTID to [9:0] | patch 0114, sleh.c |
| CPU interface bit | GICD_ITARGETSR0 byte 0 is banked and read-only: each CPU reads its own bit | GICv2 4.3.12; pe_fiq.c HEROLTE branch already relies on it |
| SPI count | GICD_TYPER.ITLinesNumber; the board's highest SPI is 0x1d1 (wdt-cl1, INTID 0x1f1) | read at start() |
| PMU (perf) SPIs | A55 n: SPI 0x90+n (INTID 0xb0+n); M3 n: SPI 0xa7+n (INTID 0xc7+n) | dts `arm-pmu` + `interrupt-affinity` (phandles 0x1b.. = cpu@000n, 0x23.. = cpu@010n) |

The driver never assumes logical CPU n = interface n: the host mock wires them scrambled on purpose.

## 2. How the driver and pexpert share the GIC

Nothing in pexpert or sleh.c changes. The split, by register:

| Register / job | Owner | When |
|---|---|---|
| GICC_CTLR, GICC_PMR (0xff), GICC_BPR (0) | pexpert `pe_init_fiq()` | every CPU, before IRQs are unmasked |
| Banked GICD_ISENABLER0 (SGI 1, PPI 27), GICD_IPRIORITYR0-7 (0xa0) | pexpert | every CPU |
| GICD_CTLR.Enable | pexpert sets it; the driver re-sets it (idempotent) | boot |
| GICC_IAR / GICC_EOIR | sleh_irq (via pexpert `a5xelte_irq_ack/eoi`) | every IRQ |
| PPI 27 dispatch (`rtclock_intr`) | sleh_irq | every timer IRQ |
| SGI dispatch (`cpu_signal_handler`, EOI first) | sleh_irq | every IPI |
| Any other INTID | sleh_irq calls the CPU's `cpu_data->interrupt_handler` = `StarbiteGICv2::handleInterrupt(intid)`, then EOIs | SPIs |
| GICD_I[SC]ENABLER1.., I[SC]PENDR1.., IPRIORITYR8.., ITARGETSR8.., ICFGR2.. (all SPI state) | **driver** | start(), registerInterrupt/enable/disable/cause, setSPITarget |
| GICD_SGIR | **driver** `sendIPI()` (AppleARMSMP `PE_cpu_signal`) | every IPI |
| Reading each CPU's interface bit | **driver**, CPU_BOOTED callback on that CPU | each CPU, once |
| Installing the per-CPU interrupt handler | **driver**, same callback (`ml_install_interrupt_handler`) | each CPU, once |

Consequences:
- The timer keeps working before IOKit and without the driver (boot CPU, early boot): nothing changes there.
- The handler that sleh_irq calls already has the INTID; the IAR was read by sleh. handleInterrupt must not touch
  GICC_IAR/EOIR. A soft-disabled level SPI is hard-disabled at its first hit (lazy disable, Apple's convention),
  so it cannot storm; an SPI nobody registered is disabled and counted ("stray").
- AppleARMSMP's `PassthruInterruptController` needs a child controller; the driver registers itself with it
  (that is what `waitForChildController()` returns, and `PE_cpu_signal` calls `sendIPI` on it). Its
  `externalInterrupt()` path is never used here: sleh_irq's A5XELTE branch runs before the USE_APPLEARMSMP branch.
- AppleARMSMP only uses `sendIPI` if every cpu node has exactly 3 interrupts (IPI, PMI, deferred IPI). The ADT
  therefore gives each cpu node `interrupts = <1 pmu 1>` with the GIC as parent (StarbitePMGR/adt-m2.diff).
  Registering INTID 1 from a cpu nub is accepted as bookkeeping (sleh dispatches SGIs itself); PPIs are refused.
- GICv2 has no deferred IPIs: a deferred one is sent at once; `cancelDeferredIPI` does nothing. The target only
  wakes early.
- A self-IPI uses the SELF filter (no mask needed), which is how each booting CPU clears its own SIGPdisabled
  before anyone knows its interface bit. Cross-CPU IPIs need the target's bit, which exists from its CPU_BOOTED
  onwards; XNU refuses to signal a CPU with SIGPdisabled before that, so the order is safe.

## 3. Choices

- All SPIs: level, priority 0xa0, routed to the boot CPU's interface at start(); `setSPITarget(intid, cpu)` routes
  one elsewhere (only to a CPU that has booted). No affinity spreading yet.
- No shared SPIs (`vectorCanBeShared` false): every board block has its own lines.
- Edge triggering is supported by the hw layer (`sb_gic_spi_set_edge`) but nothing selects it yet: no ADT
  property says which SPIs are edge. All S9 blocks the board names use level (dts flag 4) as far as checked.

## 4. Kernel symbols used that are not public KPI

Fine while the driver is built into the kernel (M1-M8). A Boot KC build needs them exported:
- `ml_install_interrupt_handler` (osfmk/arm64/machine_routines.c): the per-CPU sleh handler.
- `cpu_event_register_callback` (Private KPI), `cpu_number` (Private KPI), `gIOAllCPUInitializedKey`.

## 5. Open questions

1. GICC size: board file says 0x2000, the base ADT 0x1000. Harmless for us (no GICC_DIR, EOImode 0). Align them.
2. Is any SPI enabled by S-Boot that someone relies on? start() disables every non-secure SPI (as Linux does).
   pexpert never used SPIs and today's drivers poll, so nothing in XNU should notice; the first M2 boot will show.
3. Which SPIs are edge-triggered (if any) once real interrupt-driven drivers arrive (UFS, touch, DECON vsync)?
