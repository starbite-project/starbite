# Integrating StarbitePMGR and StarbiteGICv2 (milestone M2)

The two drivers go in together: AppleARMSMP needs both (an IOPMGR and a child interrupt controller), and the
board config that turns AppleARMSMP on also removes SweetCPU. Everything below is proposed; nothing in
build/xnu-12377 or tools/ was changed. All four diffs dry-run cleanly (`patch --dry-run -p1`) against
build/xnu-12377 (the first three) and the repo root (the last).

## 1. Patches, in order

| File (this folder) | Applies to | What |
|---|---|---|
| `board-config-two-clusters.diff` | build/xnu-12377 | STARLTE: `MAX_CPU_CLUSTERS 2`, `__ARM_AMP__ 1`, `USE_APPLEARMSMP 1`; no `NO_ECORE` |
| `kernel-two-clusters.diff` | build/xnu-12377 | sched_clutch.c 64-bit stir-the-pot registry under LL/SC; cpu_common.c no Apple CPU PIO poll on STARLTE |
| `integration.diff` | build/xnu-12377 | KernelConfigTables personalities; SweetCPU.cpp off under USE_APPLEARMSMP; gic nub published |
| `adt-m2.diff` | repo root (tools/board) | gic `#interrupt-cells = 1`; cpu `interrupts <1 pmu 1>` + `interrupt-parent` with `--topology amp`; `pmu_spi` per core |

The board-config change affects every STARLTE build: an M2 kernel needs the `--topology amp` ADT, and the M1
kernel must be built without it. Other boards (SWEET, A5XELTE, HEROLTE) are untouched: every hunk is under
`ARM64_BOARD_CONFIG_STARLTE` or `USE_APPLEARMSMP`.

Separately, the gic `#interrupt-cells` fix matters for **M1 too**: the M1 board ADT already gives arm-io blocks an
`interrupt-parent` pointing at the gic, and without `#interrupt-cells` the gic is not registered as a phandle
target, so `IODTMapOneInterrupt` calls `IODTGetICellCounts(NULL, ...)` (IODeviceTreeSupport.cpp: `AddPHandle`
needs the key; `FindPHandle` then returns NULL). Expect a data abort in IODeviceTreeAlloc, before the PE line.

## 2. Sources into the kernel

Copy (or reference) into the iokit build, and list in `iokit/conf/files.arm64` as `optional iokitcpp`:

```
StarbiteGICv2/StarbiteGICv2.cpp      StarbiteGICv2/hw/sb_gicv2.c
StarbitePMGR/StarbitePMGR.cpp        StarbitePMGR/hw/sb_smp.c        StarbitePMGR/hw/sb_psci.c
```

Each .cpp includes its own `hw/*.h` by a relative path ("hw/sb_gicv2.h", "hw/sb_smp.h"), so keep each driver's
folder layout, or add both driver folders to the include path. The hw/*.c files are plain C (stdint/stdbool/
stddef only) and compile with the iokit or bsd flags; the basenames do not collide with anything in the tree.

Both .cpp files compile to nothing useful without `USE_APPLEARMSMP` (their start() logs and returns false), so a
kernel built without the board-config diff can contain them harmlessly.

## 3. Personalities (also in integration.diff)

```
{ 'IOClass' = StarbiteGICv2; 'IOProviderClass' = IOPlatformDevice; 'IONameMatch' = 'ARM,gicv2';      'IOProbeScore' = 10000:32; }
{ 'IOClass' = StarbitePMGR;  'IOProviderClass' = IOPlatformDevice; 'IONameMatch' = 'starbite,psci';  'IOProbeScore' = 10000:32; }
```

The gic nub must be published: today's SweetPlatformExpert excludes it (`"('gic')"`); integration.diff publishes
it when USE_APPLEARMSMP. If StarbitePE (M1) replaces SweetPlatformExpert first, it must publish /arm-io/gic and
the top-level /psci node (IODTPlatformExpert::processTopLevel already publishes root children such as /psci).

SweetCPU must not run on an AppleARMSMP kernel: its personality matches every S9 cpu node (`arm,cortex-a53` is in
their compatible) and it would register the processors a second time. integration.diff drops the personality for
STARLTE and compiles the class out under USE_APPLEARMSMP.

## 4. Boot-args

- `cpus=4` (A55s only) or `cpus=8`. SWEET's default stays 1 when absent.
- `sbstop=smp` for the M2 test boot (0423 kernels): stop after the self-test, dump to pmsg, reset into TWRP.
- Nothing driver-specific otherwise. The ADT's /psci node may carry `starbite,entry-trampoline` (NOTES.md Q1).

## 5. Unchanged on purpose

pexpert `pe_fiq.c` (per-CPU GIC init, IAR/EOI helpers), `sleh.c` (timer and SGI dispatch), `start.s`
(`sweet_secondary_entry`, the bit-16 quirk), `machine_routines.c` (topology parsing). ../StarbiteGICv2/NOTES.md
section 2 is the contract between them and the drivers.

## 6. Tests

```
sh starbite/apple-drivers/StarbiteGICv2/test/run.sh
sh starbite/apple-drivers/StarbitePMGR/test/run.sh     # reads tools/board/starlte.board, uses the GIC mock
python3 tools/apple/kcheck.py starbite/apple-drivers/StarbiteGICv2/StarbiteGICv2.cpp \
    starbite/apple-drivers/StarbitePMGR/StarbitePMGR.cpp starbite/apple-drivers/StarbiteGICv2/hw/sb_gicv2.c \
    starbite/apple-drivers/StarbitePMGR/hw/sb_smp.c starbite/apple-drivers/StarbitePMGR/hw/sb_psci.c
```

kcheck uses today's config (no USE_APPLEARMSMP), so it checks only the fallback path of the two .cpp files. The
real path was checked with a wrapper that defines the board-config values before including the .cpp
(NOTES.md section 3); after the board-config diff is applied, plain kcheck covers it.
