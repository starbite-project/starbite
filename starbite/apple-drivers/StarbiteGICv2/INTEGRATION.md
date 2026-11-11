# Integrating StarbiteGICv2

StarbiteGICv2 is integrated together with StarbitePMGR (milestone M2): the same board-config switch
(USE_APPLEARMSMP) turns both on and SweetCPU off. Follow **../StarbitePMGR/INTEGRATION.md**; the parts that are
this driver's:

- Sources: `StarbiteGICv2.cpp`, `hw/sb_gicv2.c` into `iokit/conf/files.arm64` (`optional iokitcpp`).
- Personality: `IOClass StarbiteGICv2`, `IOProviderClass IOPlatformDevice`, `IONameMatch 'ARM,gicv2'`,
  `IOProbeScore 10000` (in ../StarbitePMGR/integration.diff).
- The /arm-io/gic node needs a nub (today's SweetPlatformExpert excludes it; integration.diff publishes it under
  USE_APPLEARMSMP) and these ADT properties (../StarbitePMGR/adt-m2.diff):
  - `#interrupt-cells = 1` on the gic node: without it no `interrupt-parent` reference resolves (and IODT
    crashes on the first one, which affects the M1 ADT too);
  - `AAPL,phandle` on the gic node (mkadt-board.py already adds it): the driver registers as
    `IOInterruptController%08X` of that phandle, the name IODT gives the children;
  - every cpu node: `interrupts = <1 pmu_intid 1>`, `interrupt-parent = gic` (AppleARMSMP's IPI/PMI/IPI triple;
    without exactly 3 it falls back to Apple Fast IPIs and panics).
- No boot-args. pexpert and sleh.c stay as they are (NOTES.md section 2).
