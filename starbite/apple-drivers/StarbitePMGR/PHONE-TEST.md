# M2 phone test: two clusters, IPIs, timer (StarbitePMGR + StarbiteGICv2)

**Question:** with XNU's own SMP path (AppleARMSMP) and our two drivers, do all selected cores of both clusters
come up, take IPIs and take timer interrupts?

**Pass line (pmsg):**
- run A, `cpus=4`: `StarbitePMGR: M2 PASS: cluster E: 4 up, cluster P: 0 up, IPI ok, timer 26 MHz`
- run B, `cpus=8`: `StarbitePMGR: M2 PASS: cluster E: 4 up, cluster P: 4 up, IPI ok, timer 26 MHz`

Run B only after run A passed. Each run is one boot of about a minute and ends in TWRP on its own.

## Build (host, nothing touches the phone)

1. Kernel: today's iOS 26 12377 STARLTE series through 0425 (0423 = `sbstop=`), plus, in this order:
   `board-config-two-clusters.diff`, `kernel-two-clusters.diff`, `integration.diff`, and the driver sources
   (INTEGRATION.md). A full kernel build; check the link is clean and `nm` shows `StarbiteGICv2`, `StarbitePMGR`
   and no `SweetCPU` metaclass.
2. ADT: `tools/board/mkadt-board.py BASE.adt OUT.adt --topology amp` with `adt-m2.diff` applied. Check it:
   `--check OUT.adt` passes; the gic has `#interrupt-cells`; every cpu node has `cluster-type` 'E'/'P' and
   `interrupts <1 pmu 1>`. (An M1 kernel must keep `--topology m1`.)
3. Command line: `tools/cmdline/apple-m1.txt` with `sbstop=pe` replaced by `sbstop=smp`, and `cpus=4` (run A) or
   `cpus=8` (run B). Keep `wdt=-1` (XNU's own watchdog off) and `-v`.
4. Chained test loader with its own dead-man: `sh tools/apple/m1-chain.sh PAYLOAD.xnu OUT.img 120`. The menuchain
   stub arms cluster 1's watchdog before anything else; nothing feeds it (with `sbstop=` the kernel never feeds
   the loader's watchdog, 0423). 120 s instead of 60: the self-test may wait up to 60 s for IOAllCPUInitialized
   and should get to print its FAIL line before the dead-man fires.
5. Store row: a copy of the `applem1` row (`method = "bare"`, `wdt = 0`, `wdt_feed = "own"`, `no_launch = true`,
   hidden, lab) with its own key and image. `no_launch` rows get no loader watchdog: the image's own dead-man
   (step 4) is the only one, and it is independent of XNU.

## Run (house rules)

Dry run and a verified backup first; one phone; `store-flash ... && boot ...` chained; boot by the catalog key,
never by payload name. With `sbstop=` the kernel sets `sbcrash=recovery` itself (0422): a panic, the dead-man
reset and the stop all come back to TWRP. I run everything on the host; you only press buttons if asked.

## What returns, and how to read it

Pull pmsg from TWRP (`/sys/fs/pstore/pmsg-ramoops-0`), plus the crumbs page if the run died early.
Expected lines, in order:

```
StarbiteGICv2: GIC-400 at 0x10101000/0x10102000: IIDR 0x0200143b TYPER 0x...., N INTIDs, 8 CPU interfaces, ...
StarbitePMGR: cpu 0: reg 0 mpidr 0x81000000 psci 0 kind E, XNU cluster 0 type E (boot)
StarbitePMGR: cpu 4: reg 0x10100 mpidr 0x80000100 psci 0x100 kind P, XNU cluster 1 type P      (run B)
StarbitePMGR: PSCI 0.1 over SMC, CPU_ON 0xc4000003; 8 CPUs selected; entry 0x...; PMU mapped
StarbitePMGR: PMU at 0x14060000 (+0x10000)                                                       (run B)
StarbiteGICv2: attached to the platform interrupt controller (PassthruInterruptController): 0
StarbiteGICv2: all CPUs initialized; CPU interface bits: 0:0x01 1:0x02 ...; SGI registrations 16, strays 0
StarbitePMGR: cpu 0 E mpidr 0x81000000 midr 0x41..d05. (Cortex-A55): up, IPI ok, timer 20xxx us
StarbitePMGR: cpu 4 P mpidr 0x80000100 midr 0x53...... (Exynos M3): up, IPI ok, timer 20xxx us  (run B)
StarbitePMGR: M2 PASS: cluster E: 4 up, cluster P: 4 up, IPI ok, timer 26 MHz
STARBITE: sbstop=smp: stage reached, resetting into TWRP
```

| What you see | Means | Next |
|---|---|---|
| The PASS line | M2 done for this `cpus=` | run B, or M3 |
| Nothing from StarbiteGICv2/PMGR, panic in IODeviceTreeAlloc / IODTMapOneInterrupt | the gic node lacks `#interrupt-cells` | ADT step 2 |
| "panic: Error finding interrupts for CPU n" | cpu nodes lack `interrupts` | ADT step 2 |
| "Platform does not support ACC Fast IPI" panic | cpu nodes do not have exactly 3 interrupts (`aic_ipis` false) | ADT step 2 |
| "this kernel is not built with USE_APPLEARMSMP" | board config not applied | build step 1 |
| A secondary "CPU_ON ok but NEVER CHECKED IN", all of them | entry address (NOTES.md Q1: above 4 GiB?) or the entry path | trampoline page |
| Only M3s never check in | PMU CAL not effective, or an M3 entry-path problem | compare the PMU line |
| "cpu n NOT started: PMU CAL value did not read back (... PMU offset 0x....)" | that register would not take the value | PMU block / mapping |
| "NOT started: PSCI CPU_ON failed (PSCI -2 INVALID_PARAMETERS)" | wrong PSCI target | board file psci |
| "checked in ... the core that booted is not the one described" | MPIDR/MIDR do not match the ADT | board file mpidr |
| "IPI FAILED" for a core that is up | GICD_SGIR mask or SGI delivery | the GIC interface-bit line |
| "timer FAILED" / timer far from 20000 us | PPI 27 on that core, or the timebase | pe_init_fiq on secondaries |
| No StarbitePMGR line at all, dead-man reset at 120 s | hang before or in IOKit (crumbs page) | crumbs |
| Panic text in pmsg | read the stop reason there first; never guess | |
