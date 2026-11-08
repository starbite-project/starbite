# Engineer OS v2: Starbite Lab

Engineer OS is Starbite's bench, test and management tool, its own image started from the catalog
(`docs/STARBITE-MASTER-PLAN.md` §4.2 libsb, §4.5 Engineer OS). It runs on **libsb**, the Starbite
core, as a `bare` image: the loader loads it from the OS store and jumps to it. The loader's
EngineerMode (`shim/starlte/em.c`) stays in the loader as **Engineer Lite** (hold Bixby in the
menu): the rescue tool that works when the store is broken.

**v2 (2026-11-06) is Starbite Lab**: the Engineer OS and a testing OS in one image.
- **Power, with tiers instead of a read-only fence.** Anything the hardware allows can be written:
  registers, memory, every partition and boot LU, the microSD card, the OS store, the settings
  record, the PMIC, the MAX77705 charger. Every write shows a warning screen (what happens, the
  target, old and new value, how to undo it) and then asks for the confirmation its tier needs
  (section 8). The most dangerous tier needs a second confirmation and, for storage, a verified
  backup into the OS store first. Only what is physically impossible is refused.
- **Tests** with live graphs, result pages and saved results: CPU stress on all 8 cores (both
  clusters) with checked integer / FP / NEON / cache kernels, memory stress, display stress, a touch
  grid, a storage benchmark with verify, a thermal test (every sensor vs time under load, throttling
  detection, a temperature limit), battery tests (health, discharge under load with a capacity
  estimate, voltage sag and internal resistance, charge), and a **full health check** (15 checks,
  pass / warn / fail). Every test stops on Power held.
- **Utilities**: register peek / poke, memory dump / fill, partitions and LUs (hexdump, edit, back up,
  restore, erase), the OS store (inspect, verify, copy, rename, remove), the settings editor, PMIC and
  charger writes, reboot to any target.

**v2.1 (2026-11-06, after the first use on the phone)**:
- **Sensor glitch fixed at the source.** Every thermistor read raw 0 now and then (-20 °C on the PA
  table, 90 °C on the others). The ADC code read DAT *before* it saw INT_ST set, so a conversion that
  ended between the two reads returned the value from before it (0 after the soft reset), and it also
  trusted DAT bit 31 as "done". Now INT_ST is polled alone and DAT is read after it (as the kernel's
  ISR). On top: every sensor reading (thermistors, every TMU probe, the fuel gauge's voltages) goes
  through a sample filter: impossible samples (raw 0, full scale, TMU code 0 / all ones, temperatures
  outside -30..150 °C, cell voltages outside 2.5..4.7 V) are retried and **counted on the page**, the
  value is the median of 3; a sensor that only gives impossible samples shows "no valid sample" with
  its raw value instead of a fake temperature (section 6.4).
- **"No limit"** for the temperature limit of the CPU stress, thermal, battery and GPU tests, chosen
  only through a warning that shows what still protects the SoC, read live from the TMU (section 6.1).
- **The M3 cores are offered when a test starts** (enable now, A55 only, cancel), and can be enabled
  from the CPU stress / thermal pages; once per session (section 6.1).
- **GPU tests** (Mali-G72, no OS): power-up, identification, job manager, fill pattern (also on the
  screen), stress, GPU clock (section 6.3).
- **Tier 4 (extreme)** replaces every policy refusal: S-Boot's ECT, the running Engineer OS, the secure
  world and unmapped addresses (attempted, the fault caught), raw LU writes across partitions, unknown
  PMIC blocks, DVFS outside the GPU's table. A full-screen red warning, "<target> I ACCEPT" typed, the
  slide, Power held 5 s, and for storage a backup unless "no backup" is picked on a second red screen.
  Every tier 3 / 4 action is written to the pmsg ring *before* it runs (Test history). Only what cannot
  be expressed is impossible (section 8.1).

Status (2026-11-06, v2.1): **built and checked on the host only** (526 unit checks, the reproducible
build, the relocation test, the keys-only tour of 84 pages, the Lab tour of every test including the
GPU tests on a fake Mali that walks the real page tables, a tier 4 write to the red screen). v2.0 ran on
the phone (the user's first use: CPU stress, the M3 cores after the PMU step, the sensor pages); nothing
new in v2.1 has. Section 11 has the status of each page, section 12 the phone checklist, section 10 what
is unverified, section 13 what Starbite Lab still cannot do.

## 1. Layout

| Path | What |
|---|---|
| `starbite/libsb/` | the core: `crt0.S` (header, entry, self-relocation, vectors, PSCI core stub, **worker entry and worker vectors**), `reloc.c`, `mmu.c` (v2.1: **`sb_mmu_map_temp`**, one block mapped for a tier 4 attempt), `sys.c`, `log.c`, `fdt.c`, `acpm.c` (S2MPS18; **gated `sb_pmic_write`**; v2.1 **gated `sb_acpm_dvfs`**), `hsi2c.c`, `touch.c`, `max77705.c` (**gated `sb_max_write`**; v2.1 `sb_fg_word_checked`), `tmu.c` (every probe filtered, both trips), **`adc.c`** (CMGP ADC, thermistors, v2.1 **`sb_sample_filter`**), v2.1 **`mali.c`** (the Mali-G72 driver: session, power, page tables, AS0, jobs, faults) and **`mali_desc.c`** (job descriptors, pure), `ufs.c` (**LU reads, fenced WRITE(10)**), `sd.c` (**fenced CMD24/25**), **`blkio.c`**, `store.c` (**append-only writer, settings_save, remove / rename**), `catalog.c`, **`fence.c`** (the tier fence and the authorisation gate), **`smp.c`** (the other 7 cores as workers), `crash.c`, `sb7.c` + `assets.S`, sbui (`sbui*.c`) |
| `starbite/engineer/` | the application: `main.c`, `tabs.c`, `pages_em.c` (em.c's pages), `pages_sb.c`, `dash.c`, `tests.c` (history, v2.1 tier 3 / 4 actions), **`armed.c`** (armed mode, the warning screen and confirmations, v2.1 the tier 4 red screens, backups, memory backups, restores, gated writes, tier 4 attempts, undo), **`policy.c`** (the tiers, pure), **`lab.c`** (the Lab tests), v2.1 **`gpu.c`** (the GPU tests), **`kern.c`** (the stress kernels, ARMv8.0), **`util.c`** (the utilities), `build.sh`, `preview/*.png` |
| `starbite/host/` | the host preview (`host.c` fake hardware incl. v2.1 a fake Mali and the ADC glitch, `preview.sh`, the keys-only `tour`, the **`labtour`**), `selfcheck.sh` |
| `starbite/tools/` | `mkassets.py`, `check-image.py`, `reloc-test.c`, `unit-test.c`, v2.1 `mesa-golden.c` (packs the same descriptors with Mesa 25.0.7 in the VM: the unit test's golden words) |
| `starbite/out/` | build output (ignored): `engineer.img`, `engineer.img.sha256`, `engineer.elf`, `engineer.map` |

## 2. Build

In the Lima VM (native aarch64 gcc), from the repo root or as part of the loader build:

```sh
limactl shell starbite -- sh -c 'cd <checkout>/shim/starlte && sh build.sh'   # loader, then Engineer OS
limactl shell --workdir <checkout> starbite sh starbite/engineer/build.sh     # Engineer OS alone
```

`build.sh`:
1. packs `starbite/out/sbassets.sb7` from `build/brand/menu7.sb7` (no container: a note, exit 0);
2. runs the **host unit tests** (`tools/unit-test.c`, below); a failure stops the build;
3. compiles libsb and the app with `-fpie -fvisibility=hidden -Wall -Wextra -Werror`, FP/SIMD allowed,
   `-mcpu=cortex-a55`; **`libsb/smp.c` and `engineer/kern.c` (they run on the Mongoose M3 cores) with
   `-march=armv8-a+simd`**, then disassembles them and **refuses any ARMv8.1+ instruction** (LSE
   atomics, RDM, DotProd, LRCPC, DC CVAP; the M3 lacks them; an empty listing is refused too; checked
   to catch an `ldadd`);
4. links a static PIE at 0 and the same objects at 0x40000000 for the test; refuses undefined symbols
   and any relocation other than `R_AARCH64_RELATIVE`; the pre-relocation code (`sb_mmu_enable`,
   `sb_reloc_check`, `sb_dcache_all`) must have no indirect branch or GOT access;
5. `check-image.py`: header, CRC, `text_offset` 0x80000, size budget (32 MiB, 96 MiB of arena left);
6. refuses build-host paths in the image; `reloc-test` relocates the image on the host and compares;
7. `out/engineer.img.sha256`; `--repro` builds twice and requires identical images.

**Unit tests** (`tools/unit-test.c`, 458 checks at v2.0): the string builder and CRC32; the identity
map (the lab I/O buffers non-cacheable, the secure hole unmapped); **the tier policy** (every entry of
the "most dangerous" list is tier 3 and named, peripheral clock gates and unknown blocks tier 2, known
peripherals and free DRAM tier 1, only the ECT / the running image / the secure hole impossible; an
exhaustive 4 KiB sweep of the register space: every page tier 1..3, exactly 3920 tier 3 pages and 38
tier 1 pages; memory-fill ranges; read tiers; PMIC: tier 1 only for the kernel's rail values and the
RTC; MAX77705: tier 1 only for the watchdog feed); **the fence and the gate** (the tier of every S9
partition, the boot LUs, the GPT head and tail, the live store; without a complete confirmation no
write reaches the driver at any tier or target; a confirmation covers exactly its range, LU and
medium; tier 1 does not cover BOOT; tier 3 storage needs the backup step; a widened or moved
authorisation breaks its seal; disarming voids them; dry run writes nothing; the store writer needs
an authorisation that names the store and stays inside it; an exhaustive sweep of partitions x tiers
x all step subsets agrees with the tier rule); **v2.1, 526 checks**: tier 4 (the ECT, the image, the
secure world and every unmapped address below 64 GiB are tier 4 with the consequence named, a 2 MiB
sweep of the whole 36-bit space finds no impossible address, only what cannot be expressed is; raw
writes across partitions are tier 4; tier 4 needs the phrase and the 5 s hold, tier 4 storage a backup
or the explicit waiver, the waiver never replaces a tier 3 backup; a sweep of tiers 1..4 x all 512 step
subsets over every partition and over a raw cross-partition range; the ECT through the gate), vdd_g3d
(exactly BUCK6 0x30..0x68 tier 1), unknown ACPM types tier 4, the GPU DVFS table tier 3 and anything
else tier 4, **the sample filter** (raw 0 rejected and counted, the median, a dead or open sensor gives
no sample, timeouts counted, at most 6 tries), **the Mali descriptors word for word against Mesa 25.0.7's
own packing** (WRITE_VALUE zero / imm64 chained / timestamp / cycle counter, the fragment job, two
framebuffers with their render target, the sample-position table; `tools/mesa-golden.c` packs them with
`pan_pack` and with the real `GENX(pan_emit_fbd)`, which agree; the hand packing of the iOS port, which
ran on the phone, agrees with Mesa too); **the drivers** (`ufs_write_blocks` returns -6 and
`sd_write` -9 outside the fence before touching a register; the general UFS path still accepts only
4 read opcodes); **the stress kernels** (deterministic references, every core's batch matches, an
injected corruption and a wrong reference are caught, the worker loop publishes and stops); **the
thermistor tables** (battery: ADC 1950 = 26.2 °C, S-Boot's own reading; clamps; monotonic; AP and
PA tables); the key state machine.

Output: `starbite/out/engineer.img` (v2.1: 4476928 bytes, 669 relocations; sha256
`0e12eac72c217e31b5ec031f091e18992eac1476fdb7d7f493805d7099427696`; v2.0 was 4407296 bytes,
`da25de2c...`).

### 2.1 Host self-checks (no phone)

```sh
sh starbite/host/selfcheck.sh        # build --repro + the keys-only tour + the Lab tour
sh starbite/host/preview.sh tour     # every row of every tab with Volume and Power (84 pages)
sh starbite/host/preview.sh labtour  # every Lab test started and stopped, a tier 1 write, refusals
```

The **tour** opens every row of every tab with Volume and Power only (other tabs through the
Dashboard's TABS rows), checks each page drew something and every drawn pixel reached the panel, then
holds Power four times and checks it is back at the Dashboard.

The **Lab tour** starts each Lab test with keys (CPU stress on the A55s, memory stress until stopped,
the thermal test into its load phase, the discharge and sag tests, the storage test, display stress,
the touch grid, the full health check with the M3 prompt cancelled by holding Power), holds Power
during the run and checks that the result page shows and the result is in the log; then a **tier 1
write end to end** (Save report: warning screen, Continue, slide by holding Volume Up; the entry is
appended through the fence) and **an unconfirmed write refused** (Write fence page). v2.1 adds: the
health check's **M3 offer** (enable -> the tier 3 PMU warning, cancelled -> A55 only), the **"no
limit" warning** cancelled, the **GPU tests** end to end (power-up through the tier 1 confirmation,
identification, job manager incl. the deliberate fault and recovery, the fill pattern incl. the
screen, the stress stopped with Power, the power-down putting vdd_g3d back), and **a tier 4 write**
(Register peek / poke at 0xA0000000: arm, the red screen shown, cancelled). On the host the
other cores are threads running the same kernels, the store and block devices are fakes behind the
real fence and gate, and the fake clock follows real time while the CPU is busy. The fake Mali walks
the page tables `mali.c` wrote (a wrong table or VA faults as on the GPU) and runs WRITE_VALUE and
clear-only fragment jobs; the fake ADC returns raw 0 on every 5th conversion (the phone's glitch), so
the battery tests would stop at "battery 90 °C" if the filter failed.

## 3. Entry contract (what the loader leaves, what crt0 does)

Unchanged from v1 (the loader's `boot_bare`): EL1, MMU and caches off, x0 = S-Boot's FDT, interrupts
masked, the panel scanning out 0xcc000000. crt0: own stack, FPEN, `sb_mmu_enable`, relocation,
`sb_vectors`, `sb_start` → `sb_main`.

Memory map (identity, `libsb/mmu.c`): 0..2 GiB device; 0x80000000..0x9fffffff write-back (FDT, the
loader's log page, this image at 0x92080000, its arena to 0xa0000000); 0xa0000000..0xa7ffffff
non-cacheable (S-Boot's ECT at 0xa0000000 + 0x32000, **never written**; the RAM test window
`SB_TEST_BASE` 0xa0100000, 126 MiB); 0xa8000000..0xaeffffff write-back; 0xaf000000..0xaf7fffff
non-cacheable (UFS DMA at 0xaf000000, the UFS write staging block 0xaf200000, **the lab I/O buffer
0xaf400000..0xaf6fffff and the store writer's 0xaf700000..0xaf7fffff**); 0xaf800000..0xbfffffff
unmapped (RKP, TIMA, secure world); 0xc0000000.. non-cacheable (framebuffer, pmsg, boot-next);
0x880000000.. non-cacheable (bank 2). STAGE 0x8c000000 is the loader's fastboot / POST buffer, free
while Engineer OS runs.

**Worker cores** (v2, `crt0.S sb_worker_entry`, `smp.c`): the boot core writes a start record
(index, its MAIR / TCR / TTBR0 / SCTLR, the core's 64 KiB arena stack), cleans it and the whole image
to DRAM, calls PSCI CPU_ON (A55: 1..3, M3: 0x100..0x103) and waits up to 1 s for the core to check in
before it starts the next. The core turns its MMU and caches on with the same tables (no set/way
maintenance: the A55s share the L3 with the boot core), installs `sb_worker_vectors` (any exception:
ESR / ELR / FAR into its slot, state FAULT, CPU_OFF), runs the work function, then CPU_OFF (if that
returns: state PARKED, WFE). Slots, work records and the stop flag are cleaned / invalidated by line
on both sides, so the protocol does not depend on cluster coherency. The M3 cluster first needs the
vendor PMU defaults (`xnu/patches/0304-starlte-m3-cores.patch`: PMU +0x1088 [31:24] = 0x8D, +0x2114,
+0x2180, +0x21EC, +0x2258, +0x2384 [1:0] = 2): a PMU write, so tier 3, asked once per session before
a test that uses the M3s.

## 4. Deploy (the coordinator does it)

Store entry name **`engineer.img`** (type `image`), catalog key `engineer` (method `bare`),
unchanged. The catalog's version string (`tools/store/catalog.toml`, outside this change) could become
"Starbite Lab 2.1 · tiers 1-4 · CPU / GPU stress" (the image's own About page says 2.1). Flash with
the usual store tools (pull the live store first, `--import` it: backups and reports the Lab appended
to the store are ordinary entries and are kept that way; v2.1 may also have appended `bak-ECT-<n>`
memory backups).

## 5. sbui and the keys

Unchanged from v1 (retained pages, dirty rectangles, fonts). New: `ask_hex` (Volume changes the
digit, Power moves to the next digit and then OK, hold Power cancels; touch: the upper / lower half of
a digit), `ask_pick` (a list), the warning screen, the type-the-name keypad.

Keys everywhere: Volume moves, Power selects (on release), **Power held 0.6 s = back**, short Bixby
back, long Bixby the tab's root. **In every test, Power held 0.6 s stops it** (and Bixby, a swipe
right, the back button); a Power key that was already down when the test started (the hold that
confirmed it) has to be released first, or held 3 s. Full-screen tests (display stress, touch grid,
display test, touch paint, keys) read the keys directly for the same reason.

## 6. Pages

| Tab | Pages |
|---|---|
| Dashboard | TABS (every other tab, for keys), LEAVE; live tiles (A55 / M3 MHz, hottest °C, GPU, battery, current, RAM, uptime), current graph; Device information, SoC and CPU, Memory map, Reset and boot reason, Battery graph |
| Tests | **STARBITE LAB**: Full health check, CPU stress, Memory stress, Thermal test, Battery tests, Storage test, Display stress, Touch grid, **GPU tests** (v2.1). **QUICK TESTS** (v1): Core bring-up test, Memory test, Memory bandwidth, Display test, Display panel, Brightness, Touch screen, Keys, Speed test. **RESULTS**: Test history (v2.1: with the tier 3 / 4 actions), Save report to the OS store |
| Power | Battery and charger, Battery graph, **Battery tests**, PMIC regulators, PMIC register dump, Touch rails (tier 1, real), **PMIC and charger writes**, Reboot and power |
| Clocks | CPU, Clocks, Temperatures, **Every thermal sensor**, GPU (Mali-G72), **GPU tests**, **Overclocking** (read only, section 14), Real-time clock, Timer |
| Storage | UFS host controller, UFS device and health, Partitions, GPT viewer, **Partitions and LUs**, Sector viewer (**any medium / LU, edit**), Speed test, **Storage test**, microSD card (**power and clock it up**), Card store at boot (**real settings write**) |
| Starbite | **OS store** (inspect, verify, copy, rename, remove), Catalog, Settings record, **Settings editor**, Start an OS, Write fence, Apple, iOS boot options, Android 17, Linux, Windows 11 |
| Logs | Engineer OS log, Boot log (pmsg), Loader handoff, Test history, Audit log, Retained records, Kernel command lines, Registers |
| Tools | **Armed mode** (arm, dry-run switch, undo, the tiers), **Register peek / poke**, **Memory dump / fill**, **PMIC and charger writes**, **Partitions and LUs**, **OS store**, **Settings editor**, **Save report**, Exit to the boot menu, **Reboot to** (menu, Engineer OS, any OS, Starbite Recovery, rescue, TWRP, Odin, power off), Radio / Installer ("coming"), About |

### 6.1 The Lab tests

Every test: a setup page (Power cycles an option), a live page (rebuilt 4 times a second), a result
page; the result goes to the test history (pmsg) and into the session report.

| Test | What it does | Live | Stops on | Result |
|---|---|---|---|---|
| **CPU stress** | the selected cores (all 8, the 4 A55, the 4 M3 + cpu0, cpu0) run integer (64-bit mul / mulh / div chains), FP (double Horner, sqrt, div), NEON (u32 MLA, f32 FMA lanes) and cache (512 KiB per core, address hash) kernels; every batch's result is compared with a reference the boot core computed | temperature and work-rate charts, per-core batches/s, totals, errors, faults, A55 / M3 MHz | Power, the limit (default 85 °C, 70..95, or **no limit**), the time (30 s..60 min, or never), a worker fault | PASS = no error, no fault, cores ran |
| **Memory stress** | the free window at 0xA0100000 (16 / 64 / 126 MiB, non-cacheable): zeros, ones, both checkerboards, address and its inverse, walking ones and zeros, random; write all then verify, 1 MiB at a time; a bandwidth pass (write, read, copy) per pass | pattern, progress, errors, first bad address with read / expected, bandwidth chart | Power, the pass count (1, 3, 10, never) | bad words, first bad address, MiB/s |
| **Display stress** | 10 s each: full-screen colour fills, scrolling bars, an 80 px checkerboard flip, as fast as the CPU draws (no GPU) | frame counter | Power, Bixby | fps per mode, worst frame time, the user's verdict |
| **Touch grid** | 6 x 12 cells over the panel; touched cells turn green | count | all touched, Power | cells, the first missed one |
| **Storage test** | UFS SYSTEM (or the card from block 0): N MiB sequential read timed, the same range again with every MiB's CRC32 compared, then N random 4 KiB reads; read only | MiB/s chart, IOPS, errors, mismatches | Power | MiB/s, IOPS, average / worst latency, mismatches |
| **Thermal test** | 10 s idle baseline, the CPU-stress load on the selected cores for 30 s..60 min, a 60 s cool-down; every TMU probe, the thermistors and the clocks each second | hottest chart, work chart, every sensor, throttling state | Power, the limit, a fault | start / peak / rise °C/min / cool-down; throttling (the A55 / M3 clock or the work rate 15 % below the first 10 s of load for 3 s) with time and temperature; a 1 Hz log saved with the report |
| **Battery tests** | health (every fuel-gauge value, live); **discharge under load** (all cores + full white, cores, screen, idle; 2..30 min): coulomb counting, average mA / mW, capacity estimate = mAh / SOC drop, runtime at that load; **voltage sag and stability**: 5 x (10 s idle, 10 s full load), internal resistance per cycle; **charge test** (charger in): average current, time to full, watchdog expiry, optional watchdog feed (tier 1); capacity summary | V and mA charts, SOC, mAh moved, battery temperature | Power, 3.45 V, 10 %, battery 45 °C, the CPU limit | per test, PASS rules below |
| **Every thermal sensor** | TMU_TOP probes 0..7, TMU_SUB 0..2 (each with its code and rejected samples), both decoded hardware trips, six thermistors on the CMGP ADC (raw, rejected / conversions, the ADC's completion path), the fuel gauge's temperature register and its I2C counters | live | - | - |
| **GPU tests** (v2.1) | section 6.3 | per test | Power, the limit, the time, errors, faults | per test |
| **Full health check** | 15 checks in about 30 s: timer + RTC running, PMIC (ACPM), touch controller, battery and gauge (present, model, voltage, health), battery temperature, TMUs, CPU (every core 5 s, checked), temperature under load, 32 MiB pattern test, bandwidth, UFS + both GPT headers' CRCs, UFS read + verify, OS store + catalog + engineer entry, microSD, display | a checklist | Power (the rest: "not run") | pass / warn / fail per check |

Battery PASS rules: discharge: samples taken and the voltage stayed ≥ 3.3 V; sag: 5 cycles, average
internal resistance < 300 mΩ, minimum ≥ 3.3 V; charge: average > 50 mA and no watchdog expiry.

**The temperature limit and "no limit" (v2.1).** "Stop at" cycles 70, 75, 80, 85, 90, 95 °C and
**no limit**. No limit is chosen only through a tier-1 warning (warning screen, slide) that reads the
TMU live and says what still protects the SoC: whether S-Boot armed the hardware trip (TMU CONTROL bit
12, THERM_TRIP_EN) on TMU_TOP and TMU_SUB, and the threshold it programmed (THD_TEMP0 [24:16], decoded
with probe 0's trim). What is verified: those two registers (the host model and S-Boot's disassembly
both give 115 °C; the phone's value is shown live). What is **not** verified: that the trip fires, what
it does (the vendor driver's description: the TMU signals the PMU, which cuts the power) and which
probes it watches. If no trip is armed the warning says that only the user stops the heat. The same
option is in the thermal test, the battery tests (their CPU limit) and the GPU stress. With a limit
set, 3 s without one valid TMU sample also stops the load (a limit that cannot see is no limit); the
battery tests fall back to the fuel gauge's temperature register when the battery thermistor has no
valid sample.

**The M3 cores (v2.1).** A test that uses them (CPU stress, thermal, battery, health check) offers
them when it starts: **Enable the M3 cores now** (tier 3: the 6 PMU registers, as before; worked on the
phone on 2026-11-06), **Run on the A55 cores only**, or **Cancel**. Once enabled they stay enabled for
the session; the CPU stress and thermal pages also have an "M3 cores" row to enable them up front.

**Results saved**: each result is a test-history line in the pmsg ring (as v1: survives a warm
reset). **Save report to the OS store** (Tests, Tools, the health check) appends one text entry
`lab-YYYYMMDD-hhmmss.txt` to the UFS store (else the card's): every result line of the session, the
health checklist, the thermal log (1 Hz CSV) and the Engineer OS log; tier 1 (warning, slide).

### 6.2 The utilities

| Utility | What |
|---|---|
| Register peek / poke | any address: 16 rows live; reads free in the known blocks, others ask once per 64 KiB block; a 32-bit write at the tier of the address (section 8); read back; a fault or a pending SError is reported; Undo writes the old value back (same tier) |
| Memory dump / fill | bytes + ASCII of any DRAM; fill a range with a 64-bit pattern (the range's worst page sets the tier; S-Boot's ECT and Engineer OS itself are impossible) |
| Partitions and LUs | every GPT partition, boot LUs 1..4 (size from the unit descriptor), the whole card: hexdump (sector viewer), back up to the OS store (`bak-<NAME>-<n>`, verified), compare with a backup, restore from a backup, erase (zeros, at least tier 2) |
| Sector viewer | any medium and LU; next / previous / ±256 / jump; edit 4 bytes at an offset (the fence's tier; tier 3 backs up that block first) |
| OS store | both stores: header, entries; per entry: verify the CRC, hexdump, copy to the other medium's store (append, verified), rename and remove (tier 2: the directory, then the header) |
| Settings editor | default OS, timeout (0..60 s), **POST level and log level** (stored in the record's flags, bits 4-5 and 8-9; today's loader does not read them yet), card store at boot, next boot (once), last choice; Save (tier 1, read back), Revert |
| PMIC and charger writes | any S2MPS18 (TOP / PMIC / RTC) or MAX77705 (charger, gauge, USB-C, PMIC) register; tier from `pmic_tier` / `max_tier`; read back; Undo |
| Reboot to | the boot menu, Engineer OS again, any catalog entry (boot-next), Starbite Recovery (a reboot with "hold Bixby"), the rescue system (boot-next `rescue`), TWRP, Odin, power off |

### 6.3 GPU tests (v2.1)

Tests > GPU tests (also Clocks > GPU tests). The Mali-G72 MP18 without any OS (`libsb/mali.c`,
`mali_desc.c`, `engineer/gpu.c`), after the recipe Starbite's iOS port ran on this phone (NOTES.md
2026-10-28: WRITE_VALUE jobs, fragment clears on the panel, a triangle, full-frame presents at 260 and
572 MHz with vdd_g3d at 950 mV). Interrupts are polled; every wait is bounded.

| Row | What it does | Writes |
|---|---|---|
| Power up the GPU | checks the G3D domain (PMU 0x14064044 = 0xF; if off, offers the tier 3 power-on with the TZPC SMC, never run), GPU_ID 0x6221, PLL_G3D locked; vdd_g3d (S2MPS18 BUCK6, reg 0x2B) to 950 mV; soft reset; L2, the 18 shader cores, the tiler; Mali-LPAE tables and 8 MiB of data in DRAM bank 2 at 0x8F0000000 (32 MiB nothing else uses; the GPU sees only that and the screen); AS0 | BUCK6 (tier 1: 600..950 mV is tier 1, everything else tier 3), then GPU registers and bank 2 under that confirmation |
| Identification and registers | GPU_ID / revision, shader cores (18 expected), L2 and MMU features (VA / PA bits), AS and job slots with each slot's features, read-only IDs read twice; powered: the IRQ masks written and read back, the GPU cycle counter over 20 ms of the CPU timer (its real clock vs the PLL), the GPU timestamp vs the CPU counter | IRQ masks (restored), cycle counter on / off |
| Job manager test | a WRITE_VALUE on every slot that takes one (a 16 x 16 fragment job on a slot that does not), the value types (zero, imm8 / 16 / 32 / 64 with the bytes around kept, the system timestamp) as one chain, a chain of 128 jobs (every value checked, jobs/s), the job interrupt seen pending at the GIC (GICD 0x10101224 bit 26, SPI 282), a deliberate MMU fault (a write to an unmapped GPU VA: the translation fault must be reported) and the recovery (reset, cores, AS0, a job runs again) | jobs, JOB_IRQ_MASK for one job |
| Fill pattern test | 64 clear-only fragment jobs paint an 8 x 8 colour grid into a 256 x 256 buffer, one job at a time and then as one chain; every pixel checked on the CPU (a red / blue swap is named as a descriptor channel-order issue); then a 512 x 512 grid straight into the scanout at (464, 1216), read back from the scanout and shown until a key (8 s) | GPU memory, the screen |
| GPU stress | rounds of one full-buffer fragment job (512² or 1024²) on JS0 and a 64-job WRITE_VALUE chain on another slot at the same time; 64 random pixels and all 64 values checked each round; Mpixel/s, jobs/s, the GPU's TMU zone (TMU_SUB probe 1) and the hottest probe, vdd_g3d, the clock; charts | jobs |
| GPU clock | ACPM DVFS (domain g3d = 4) to a level of the vendor DT's table (exynos9810.dtsi `gpu_dvfs_table`: 260, 299, 338, 455, 546, 572 MHz): tier 3; if PLL_G3D is not locked afterwards, 260 MHz is asked again; if ACPM changed vdd_g3d, offers 950 mV again | ACPM DVFS (tier 3; any other domain or level tier 4) |

Stops: Power held (between steps for the one-shot tests), the time, a wrong pixel or value, a fault,
the temperature limit (or none, after the same warning). Every failure shows the slot's JS_STATUS
(with the exception name), JOB / MMU / GPU IRQ_RAWSTAT, AS0 FAULTSTATUS and FAULTADDRESS, and
GPU_FAULTSTATUS / FAULTADDRESS; the GPU is reset before the next job. **Leaving the GPU page powers the
cores down and puts vdd_g3d back to S-Boot's value** (with the power-up's own tier 1 confirmation).
Descriptors: Mesa genxml v6 layouts (MIT), checked word for word against Mesa's own packing in the unit
test; the register map is Linux Panfrost's (GPL, reference only).

### 6.4 Sensor readings (v2.1)

| Sensor | How it is read | Rejected (counted on Every thermal sensor) |
|---|---|---|
| CMGP ADC (6 thermistors) | the kernel's sequence; INT_ST polled alone (2 ms), DAT read **after** it; if INT_ST never comes, DAT once after 2 ms, used only with its bit 31 (counted as "completion not signalled"); median of 3 accepted conversions out of 6 at most | raw 0 and 4095 (shorted / open / read too early), timeouts; the page shows rejected / conversions and the last rejected raw value; a sensor with no valid sample shows that and its raw value, never a table-end temperature |
| TMU_TOP / TMU_SUB (11 probes) | CURRENT_TEMP has no valid bit (exynos_tmu.c reads it as plain status): 3 samples 10 µs apart, median | code 0, code 0x1FF, temperatures outside -30..150 °C; rejected count and last rejected code per probe |
| Fuel gauge (MAX77705) | every word read up to 3 times on a bus error | VCELL / AvgVCELL / OCV outside 2.5..4.7 V, VSYS outside 2.5..5.5 V, RepSOC above 101 %; a sample without a valid cell voltage is "not read" instead of 0 mV (which the discharge test took for an empty cell); reads / retries / failures / implausible counted |

## 7. Host preview

```sh
sh starbite/host/preview.sh            # all scenes -> starbite/engineer/preview/*.png
sh starbite/host/preview.sh cpu        # one scene (cpu, health, warning, sensors are new in v2)
```

The real app and sbui compiled for aarch64 Linux on `starbite/host/host.c`: register blocks, DRAM
windows, pmsg and the loader's log page are mappings at their real addresses with seeded values; the
PMIC, fuel gauge, touch, UFS, store, card and ADC are fakes. v2: the other cores are threads; the TMU
codes follow a simple thermal model of the threads' load; written blocks are kept (so a backup and a
compare see the same data); the store fake's writer goes through the real fence and gate.

## 8. Tiers, confirmations and armed mode

The policy is `engineer/policy.c` (registers, memory, PMIC, MAX77705) and `libsb/fence.c` (storage).
Nothing is refused because it is risky; a riskier target needs more confirmation.

| Tier | Confirmation | Targets |
|---|---|---|
| 0 | none | every read of a known block, DRAM outside the carve-outs |
| 1 | warning screen + **held** confirmation (slide, or hold Volume Up 1.5 s) | OS partitions (SYSTEM, VENDOR, ODM, A16DATA, WINESP, WINDOWS, USERDATA outside the live store, CACHE, OMR, HIDDEN, CP_DEBUG), the microSD card, store appends (backups, reports), the settings record; known peripheral registers (pin controllers, the touch / MAX77705 I2C buses, the ADC, DECON, DSIM); free DRAM (incl. the GPU tests' 32 MiB at 0x8F0000000), the framebuffer, pmsg; the touch and card rails at the kernel's values; **vdd_g3d (BUCK6) 600..950 mV** (v2.1: the GPU's known-good value); the RTC; the charger watchdog feed; "no temperature limit"; reads of unknown registers (once per block) |
| 2 | + the target's name **typed** on the keypad | BOOT, RECOVERY, UP_PARAM, RADIO, partitions Starbite does not know, the live OS store (raw writes, rename / remove), erasing a whole partition; peripheral CMU gates (PERIS, PERIC0/1, FSYS0/1, APM, CMGP, DPU, AUD, the card gate in CMU_TOP), the UFS host and PHY, the card controller, the watchdog, the GIC, the always-on GPIOs (the keys), SYSREG_CMGP, the Mali GPU's registers (its jobs write memory by DMA), any unknown device address; the UFS DMA window; S-Boot's FDT |
| 3 | + a **second confirmation** (hold Power 3 s), and for storage **a verified backup first** | EFS, CPEFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA0-2, DTB, DTBO, the GPT/PIT head and tail, **boot LUs 1..7**; **PMU** (power, reset, DVFS, CPU power-up, incl. the M3 sequence), **CMU_TOP / CORE / BUSC (MIF, INT)**, **MIF (CMU_MIF, DREX/DMC, DDR PHY)**, **CPU clusters (CMU_CPUCL0/1, DVFS)**, **CMU_G3D**, the ACPM / APM mailbox and APM SRAM, **CHIPID / fuses**, the **TMU** (S-Boot's 115 °C trip), every PMIC regulator / BUCK / TOP register or value other than the rails above, every MAX77705 register other than the watchdog feed (incl. CHG_INT since v2.1: a status register the chip should ignore), the secure-camera and modem carve-outs (may raise an SError), the G3D power-domain switch, **the GPU clock through ACPM DVFS** at a level of the vendor table |
| **4 (extreme, v2.1)** | a **full-screen red warning** naming the consequence and the recovery (Cancel is the default button), then **"<target> I ACCEPT"** typed (letters, digits, `_`, space), the slide, **Power held 5 s**; storage: a verified backup first unless **"NO BACKUP: I accept the loss"** is picked on a **second red screen** (Back up first is the default) | **S-Boot's ECT** (0xA0000000..0xA00FFFFF: Android's kernel reads it at boot, a bad ECT can make Android panic; a verified copy into the OS store (`bak-ECT-<n>`, 1 MiB) is offered first and where it went is shown; S-Boot reloads the ECT from flash when it starts, so a full restart most likely restores it, not verified), **the running Engineer OS** (0x92000000..0x9FFFFFFF: Lab crashes or misbehaves at once; if the screen freezes, hold Power + Volume Down 10 s, the PMIC resets the phone and the loader starts again), **the secure world** (0xAF800000..0xBFFFFFFF) and **every address Engineer OS leaves unmapped** below 64 GiB (the block is mapped Device-nGnRnE for that one access, so it really reaches the bus; TrustZone / the interconnect faults it, Lab's exception handler catches the synchronous abort and reports ESR / FAR and any pending SError instead of hanging; EL3 or a hypervisor may instead reset the phone, which then returns to the loader with nothing changed on storage), reads of those addresses (Register peek: "Attempt a read here"), **raw LU0 writes** across a partition boundary, between partitions or without a readable GPT, ACPM PMIC types 3..15 (blocks Starbite does not know), ACPM DVFS outside the GPU's vendor table (other domains: an ACPM MIF / INT request froze the phone under iOS) |
| not possible | cannot be expressed (section 8.1) | addresses at or above 64 GiB; a PMIC register outside ACPM's 4-bit type / 8-bit register / 8-bit value; an I2C address above 0x7F; an empty or wrapping range; a block medium that does not exist |

### 8.1 What is still refused, and why (hardware facts only)

| Refused | Why it is not a policy |
|---|---|
| A physical address at or above 64 GiB (0x10_0000_0000) | Engineer OS's MMU covers a 36-bit address space (TCR T0SZ = 28: 64 GiB of VA, identity-mapped); there is no virtual address to issue the access from, and nothing of the S9's memory map lives there (DRAM ends at 0x9_0000_0000) |
| A PMIC type above 15, a register or value above 0xFF | the ACPM PMIC command carries a 4-bit type and 8-bit register and value fields: the request cannot be written |
| An I2C address above 0x7F, a MAX77705 register or value above 0xFF | 7-bit I2C addressing, 8-bit registers |
| An empty or wrapping range, a storage medium other than UFS / microSD | not a write at all |
| Writes to secure memory *succeeding* | not refused: the attempt is made (tier 4); TrustZone (TZASC / EL3) denies a non-secure master, so the store faults or is dropped, and Lab reports what came back |

**The flow** (`armed.c lab_confirm`), for every write:
1. the **warning screen**: the tier, what happens (one line per step), the target, now / after, how
   to undo it, the backup, which confirmations follow; Continue or Cancel (hold Power = cancel);
2. tier 2 and 3: **type the target's name** on the keypad (letters, digits, `_`, space; Volume moves,
   Power types, hold Power cancels);
3. the **held confirmation**: a sheet with what / target / now / after / backup / undo / tier and a
   slider (drag it, or hold Volume Up 1.5 s);
4. tier 3: the **second confirmation**, hold Power 3 s (red ring; Bixby cancels);
5. tier 3 storage: **`lab_backup`** copies the target range into the OS store (UFS, else the card's)
   as `bak-<NAME>-<n>`: every chunk read back and compared, the whole entry CRC'd again, then the
   source read once more and compared with the backup; the dialog and the audit log say where it went;
6. the write runs with the **gate** open on the authorisation; the write is read back.
Tier 4 (v2.1) replaces 1-4 with: the red warning, the typed phrase "<target> I ACCEPT" (records both the
typed-name and the phrase steps), the slide, Power held 5 s (records the second-confirmation and the
5 s steps), and for storage the second red screen (backup first, or the waiver step); the backup runs
as for tier 3 unless waived (`lab_backup_needed`).
Below 20 % battery without a charger every write is refused; if the gauge does not answer the user
is asked.

**The record of tier 3 / 4 actions (v2.1).** Every tier 3 or 4 confirmation is written to the pmsg ring
*before* the action runs, as `SBENG1 W <what> | <RTC date and time>, confirmed tier N: <target> now
<old> after <new> [(NO BACKUP ...)]`, and every outcome (written / FAILED / FAULTED with ESR and the
SError state) as `... outcome: ...`. The ring survives a warm reset (a tier 4 write that resets the phone
leaves its confirmation there); Tests > Test history lists them (this session and earlier ones) under
TIER 3 / 4 ACTIONS, TWRP shows them in `/sys/fs/pstore/pmsg-ramoops-0`, and Save report copies this
session's.

**The authorisation** (`fence.c struct sb_auth`): space (UFS / SD / memory / PMIC / MAX77705 / ACPM DVFS), LU,
range, tier, kind and the steps passed, sealed with a per-boot secret. One authorisation is open at a
time (the gate) for one confirmed action. `sb_write` writes only if the open authorisation covers the
whole range on that medium and LU, has the tier the fence asks for and every step that tier needs;
`sb_pmic_write`, `sb_max_write` and `lab_poke32` check the same gate with their policy's tier. The
UFS and SD drivers refuse a write that does not come from `sb_write`. Disarming (or a reboot)
voids every authorisation. The store writer (`sb_write_store`) needs an authorisation of kind STORE
and stays inside the store's region; a backup taken as part of a confirmed action gets its own
tier 1 STORE authorisation from that confirmation.

**Armed mode** is the session switch for the **free-form** tools (register poke, memory fill, sector
edit, PMIC / MAX77705 writes): hold Power 2 s, then slide; a red border, "ARMED · m:ss" in the status
bar; it ends after 5 minutes without input, when the page it was armed on is left, and on reboot.
Named actions (a backup, a restore, the settings, a report, the rails, the M3 sequence) carry their
own warning and confirmation and do not need it. **Dry run** (Armed mode page) makes every storage
write a logged no-op, for rehearsals. **Undo** writes the last poked register / PMIC / MAX77705
value back (at the same tier). Every confirmation, cancellation, write, backup and refusal is in the
**audit log** (and the Engineer OS log, and pmsg).

**The OS store writer** (`store.c`): append only. New data beyond the used end, read back; the new
directory entry in slot n, which the header's directory CRC (over n entries) does not cover yet; then
block 1 with n + 1, the new used end and both CRCs: the single write that makes the entry exist. A
failure before it leaves the store as it was. The tail guards of `sbstore2.py` are kept (UFS: the
last 1025 blocks of USERDATA, iOS 26's herolog ring; card: the last 16384 blocks). Rename / remove
rewrite the directory and then the header (tier 2: a power loss between the two writes leaves the
loader on the version 1 view); the old directory is written back if the header write fails; removed
data stays until a repack on the Mac.

## 9. What Engineer OS writes on its own, and what only after a confirmation

On its own (the loader's own behaviour, as v1): the ACPM IPC queue, the RTC read latch, HSI2C
controller registers, the touch and MAX77705 pin mux / SYSREG USI mode when they differ, UFS host
setup, the vs_hci registers during the v1 speed test (restored), the DECON trigger, pmsg, the
boot-next mailbox, PMU reset / PS_HOLD, PSCI CPU_ON / CPU_OFF; **v2**: the CMGP ADC's own registers
during a conversion (soft reset, C_TIME, channel, start, INT_EN / INT_ST: what the kernel's driver
does), the RAM test window during memory tests, the cache-maintenance of the worker protocol;
**v2.1**: the GPU cycle counter on / off and nothing else on the GPU without a session.

Only after the user confirmed it on the phone: every block write (UFS any LU, the card, the store,
the settings record), every register / memory write outside the above, every PMIC and MAX77705
write, the M3 PMU sequence, the card's clock gates and rails, the touch rails; **v2.1**: vdd_g3d and,
under that "Power up the GPU" confirmation (`sb_gpu_open` refuses every GPU register write without
it), the GPU's registers (reset, power, AS0, IRQ masks, job slots) and its 32 MiB in bank 2; the GPU
clock (ACPM DVFS); a temporary MMU mapping for one tier 4 access (`sb_mmu_map_temp`).

## 10. Untested and unsure (read before the first boot of v2.1)

New in v2.1, never run on the phone:
- **The ADC fix**: that INT_ST bit 0 rises on this ADC when polled with INT_EN set (the kernel's ISR
  waits for exactly that; v2.0 also accepted DAT bit 31, so which of the two gave the good readings is
  not known). If INT_ST never comes, the page says "INT_ST did not come n times: DAT bit 31 used" and
  every conversion takes 2 ms; if the raw-0 readings persist, the rejected counters show them and the
  median hides them, but then the cause is elsewhere (please photograph the counters).
- **The TMU filter**: a probe that only gives code 0 / 0x1FF shows "no valid sample", and a limit then
  stops the load after 3 s. That every probe can glitch at once (as the user saw) is plausible but
  its cause is unknown (no valid bit is documented); the filter handles it, it does not explain it.
- **GPU tests**: everything (section 6.3). The recipe is the one that ran under iOS on this phone, but
  without an OS it has never run: the BUCK6 write from Engineer OS's ACPM path (the same IPC as the
  touch rails), the soft reset from S-Boot's state, the job slots' features (the host assumes JS0 takes
  fragment + WRITE_VALUE, JS1 / JS2 WRITE_VALUE; the test reads the real ones and adapts), chained
  fragment jobs (Panfrost submits one per chain: if the chain step fails while single jobs pass, the
  chain is not supported and the stress does not use it), the GIC pending check (GICD 0x10101224
  bit 26 for SPI 282), the deliberate MMU fault's exact status code, the cycle counter (only counting
  while the GPU is powered), the GPU clock levels other than 260 and 572 MHz, and the G3D domain
  power-on path (S-Boot leaves the domain on; the SMC was taken from the S9+ Linux port, never run
  here). The fill test writes into the scanout at (464, 1216): the square stays until a key.
- **Tier 4 attempts on unmapped / secure addresses**: the temporary mapping is Device-nGnRnE of a
  2 MiB (bank 1) or 1 GiB block, TLB invalidated inner-shareable. What the S9's TrustZone does with a
  non-secure access to RKP / TIMA memory (a synchronous external abort, an SError, a silent drop, or an
  EL3 / hypervisor reset) is unknown; each is caught or reported, a reset leaves the confirmation line
  in pmsg.
- **The ECT memory backup** (`bak-ECT-<n>`, 1 MiB) and the claim that S-Boot reloads the ECT at every
  start.
- **The red screens and the phrase keypad** on the real panel (drawn and navigated on the host only).

From v2.0 (the user's first use ran the CPU stress, the M3 cores after the PMU step and the sensor
pages; the rest below is still untried). The risky parts, in the order to try them:
- **Worker cores**: the worker entry turns the MMU on with the boot core's TTBR0 / TCR / MAIR / SCTLR
  and no cache maintenance of its own. The A55s have run PSCI stubs (v1 core test); C code with the
  MMU on has not. The M3s have never checked in under the loader's stub (NOTES.md 2026-10-25); iOS
  needed the PMU sequence first (0304), which Engineer OS now writes (tier 3). Coherency between the
  clusters is not assumed (line maintenance everywhere, the image cleaned before every CPU_ON). A
  worker that faults reports ESR / ELR and turns off; a worker that never checks in shows "no check-in
  in 1 s"; a core that cannot be stopped is logged ("reboot before starting an OS").
- **Thermal throttling without a kernel**: whether the APM firmware lowers the CPU clocks on its own
  is unknown (build/em-research-thermal-clocks.md item 7). The thermal test detects either outcome;
  the 85 °C default limit stops the load long before S-Boot's 115 °C trip.
- **UFS writes**: the WRITE(10) path is the loader's settings writer (one block per command), which
  works on the phone; multi-block use, writes to other LUs (boot LUs may be write-protected: the
  error is shown) and UFSP-protected ranges are untried.
- **microSD writes** (CMD24 / CMD25, FIFO filled by the CPU, busy and CMD13 waits): new code, never
  run. Try it on a card with nothing on it first.
- **CMGP ADC**: registers from the vendor driver (exynos-adc-v3), polled instead of the interrupt;
  the channel numbers (battery 4, charger 3, USB 1, AP 0, Wi-Fi 2, PA 10, from the vendor DT) are
  unverified (the values the phone showed were plausible). The gate 0x14202018 (ADC_CMGP PCLK_S0) is only
  read; whether S-Boot leaves it open is unverified. If the ADC does not answer, the battery tests
  fall back to the fuel gauge's temperature register (S-Boot's last write, labelled so).
- **Fuel gauge**: Cycles (0x17) is shown as raw / 100 (ModelGauge LSB 1 %): unverified for the
  MAX77705. IIN (0xD0) and VSYS (0xB1) on the gauge address are from the Linux driver's comments.
  The charger watchdog feed writes CNFG_06 [1:0] = 01 keeping CHGPROT: as Linux, unverified here.
- **TMU trip decode** (THD_TEMP0 0x50 [24:16] with probe 0's trim) and TMU_TOP probes 0 / 7, TMU_SUB
  probe 0 (unassigned in the DT): shown, physical location unknown.
- **Card bring-up**: opening the CMU gates 0x1A242070 / 0x11402030 / 0x11402034 (bits 20 and 21, as
  Linux's s9p-clkgate) and the rails power cycle (the loader's hw_sd_rails) from Engineer OS.
- **POST level / log level** are stored in the settings record's flags (bits 4-5, 8-9); today's
  loader keeps the flags but does not act on them.
- **Display stress**: the CPU fills 17 MB per frame; the frame rate on the A55 has never been
  measured. Bright fields are not static (no burn-in), but the fills run 30 s.
- From v1: the touch re-init without CMU writes, the MAX77705 bus without CMU_CMGP writes, frame
  rate, the menu7.c Lab row.

Register and sensor addresses used and how sure they are:

| Address | What | Source | Sure? |
|---|---|---|---|
| TMU 0x10070000 / 0x10074000: TRIMINFO 0x00+4p, CONTROL 0x20, CURRENT_TEMP 0x40/0x44/0x48, THD_TEMP0 0x50, EMUL 0x160 | temperatures | vendor exynos_tmu.c + DT + S-Boot disassembly (build/em-research-thermal-clocks.md) | yes (v1 reads them); trip decode no |
| PLL_CPUCL0 0x1D000140, CPUCL1 0x1D100120, muxes / dividers | clocks (throttling) | cmucal (same research) | yes (v1) |
| ADC 0x14230000 (CON1 0, CON2 4, DAT 8, INT_EN 0x10, INT_ST 0x14), gate 0x14202018 | thermistors | vendor exynos_adc.c (v3), cmucal-sfr.c | registers yes, completion bit and gate state no |
| MAX77705 FG 0x36: 0x00, 0x05, 0x06, 0x08, 0x09, 0x0A, 0x0B, 0x0E, 0x17, 0x18, 0x19, 0x23, 0x35, 0xB1, 0xD0, 0xFB, 0xFF; charger 0x69: 0xB2, 0xB4, 0xBD | battery | Linux max77705 drivers (build/em-research-battery.md) | mostly; 0x17 scale, 0xB1 / 0xD0 no |
| PMU 0x14061088, 0x14062114/2180/21EC/2258/2384 | M3 power-up defaults | xnu patch 0304 (Linux S9+ port, measured) | yes on iOS / Linux; not under Engineer OS |
| PSCI 0xC4000003 CPU_ON, 0x84000002 CPU_OFF; targets 1..3, 0x100..0x103 | cores | NOTES.md 2026-10-25 / 10-28 | yes |
| CMU 0x1A242070, 0x11402030, 0x11402034 bits 20-21 | card clocks | s9p-clkgate, loader sd.c | yes (Linux, the loader) |
| S2MPS18 0x6C / 0x74 = 0xEC / 0xF0, 0x68 / 0x4A = 0xE8 / 0xFC | touch / card rails | the loader's menu.c | yes (the loader writes them) |
| UFS LUs 1..4 = the boot LUs | boot LU reads | master plan ("sdb-sde") | LU numbering unverified |
| Mali 0x17500000 (GPU_ID 0, L2/CORE/TILER/MEM/MMU features 4..0x14, AS/JS present 0x18/0x1C, IRQ 0x20..0x28, COMMAND 0x30, FAULT 0x3C/0x40, CYCLE 0x90, TIMESTAMP 0x98, THREAD 0xA0, JS_FEATURES 0xC0+4n, PRESENT / READY / PWRON / PWROFF 0x100..0x1E0, JOB_IRQ 0x1000.., JS n 0x1800+0x80n, MMU_IRQ 0x2000.., AS0 0x2400) | the GPU tests | Linux Panfrost regs, xnu/src/starlte_gpu.c (ran on the phone) | yes (iOS); cycle / timestamp registers not run here |
| Job descriptors (WRITE_VALUE, FRAGMENT, FBD + RT, sample positions) | the GPU tests | Mesa genxml v6, checked against Mesa's packing (unit test) | yes (iOS ran the same words) |
| BUCK6 0x2B = 0x68 (950 mV), S-Boot ~0x33..0x37 | vdd_g3d | NOTES.md 2026-10-28, s9p-g3d | yes (iOS, Linux) |
| PMU 0x14064040 / 0x14064044, SMC 0x82000410(1, 0x17410204, 0) | G3D domain, TZPC | the S9+ Linux port (s9p-g3d-pd) | status yes; the power-on path never run |
| ACPM DVFS { 4, kHz, 0, 0 } on channel index 0 | GPU clock | xnu 0303 (572 MHz worked); vendor exynos9810.dtsi levels | 572 MHz yes on iOS; others no |
| GICD 0x10101000 (ISPENDR9 0x224 bit 26 = SPI 282) | the job IRQ check | vendor exynos9810.dtsi (GIC-400), INTREQ__G3D_IRQJOB 282 | the address yes; the check never run |
| TMU_SUB CONTROL bit 12, THD_TEMP0 | the SUB trip | S-Boot programs both TMUs (research 1.4) | the decode as TOP's; not run |

## 11. Status per page and test (v2.1)

"HW" = seen working on the phone; "host" = built, drawn and navigated in the host preview and the
tours, not yet looked at on the phone; "host+lab" = also started and stopped by the Lab tour.

| Page / test | Status | Writes |
|---|---|---|
| Start-up, relocation, MMU, crash vectors | **HW** (v1.1) | - |
| Touch input | **HW** (v1.1) | - |
| Keys-only navigation, TABS rows | host (tour: 84 pages); HW for v2.0's pages (first use) | - |
| Dashboard, device, SoC, memory map, reset, battery graph | host | - |
| Full health check | host+lab (v2.1 with the M3 offer) | RAM test window; M3 PMU sequence (tier 3, offered) |
| CPU stress | **HW** (v2.0: A55, then all 8 after the PMU step); v2.1 host+lab (offer, no-limit warning) | PSCI; M3 PMU sequence (tier 3, offered) |
| GPU tests (power-up, identify, job manager, fill, stress, clock) | host+lab (fake Mali; clock not toured) | vdd_g3d (tier 1), GPU registers and bank 2 (under that), ACPM DVFS (tier 3), the scanout |
| Memory stress | host+lab | the RAM test window |
| Thermal test, Every thermal sensor | sensors **HW** in v2.0 (showed the raw-0 glitch); v2.1 host+lab (filter, glitch emulated) | ADC conversions |
| Battery tests (health, discharge, sag, charge) | host+lab (discharge, sag) | charger watchdog feed (tier 1, opt-in) |
| Storage test | host+lab | - (reads only) |
| Display stress | host+lab | framebuffer |
| Touch grid | host+lab | - |
| Save report to the OS store | host+lab (tier 1 write end to end) | store append (tier 1) |
| Quick tests (core bring-up, memory, bandwidth, display, panel, brightness, touch, keys, speed) | host (v1 code) | as v1 |
| Test history | host | pmsg |
| Battery and charger, PMIC regulators / dump | host | - |
| Touch rails | host | PMIC LDO35 / LDO43 (tier 1) |
| PMIC and charger writes | host | any PMIC / MAX77705 register (tier 1..4) |
| Clocks, CPU, temperatures, GPU, RTC, timer | host | RTC read latch |
| UFS pages, GPT viewer | host | - |
| Partitions and LUs (hexdump, back up, compare, restore, erase) | host | UFS / card (tier 1..3), store appends |
| Sector viewer (any medium / LU, edit) | host | one block (fence tier) |
| microSD card (power, clocks, retry), Card store at boot | host | PMIC rails (tier 1), CMU gates (tier 2), settings (tier 1) |
| OS store (verify, copy, rename, remove) | host | store append (tier 1), directory rewrite (tier 2) |
| Settings editor | host | the settings record (tier 1) |
| Write fence page | host+lab (unconfirmed write refused) | - |
| Start an OS, Reboot to (all targets), Exit | host | boot-next mailbox, PMU reset |
| Armed mode, dry run, undo, audit log | host | - |
| Register peek / poke, Memory dump / fill | host (+lab: a tier 4 write to the red screen) | any address (tier 1..4), tier 4 reads |
| Tier 4 red screens, phrase, 5 s hold, backup choice, ECT backup | host (red screen in the Lab tour; the rest only by reading the code) | - |
| Logs | host | - |

## 12. Phone checklist (Starbite Lab v2.1)

Start it from the boot menu's Lab row / dock (or `tools/store/boot-next.sh engineer`). Only the
phone's buttons and screen are needed; tick what works, note what does not. Charge the phone above
50 % first. **Steps A-G and 1-9 write nothing to storage.**

New in v2.1 first:

A. Clocks > **Every thermal sensor**, watch it for a minute: no thermistor jumps to -20 / 90 °C any
   more. Note the "rejected n of m" counters, the "ADC completion" line (INT_ST, or "DAT bit 31 used")
   and each TMU probe's "rejected" count (photo). Rejections with steady values = the filter works;
   none at all = the INT_ST fix alone cured it.
B. Tests > **CPU stress** with "all 8": Start offers **Use the M3 cores?** Pick Enable (type PMU,
   slide, hold Power 3 s); the 8 rows run. Start again: no offer (enabled for the session).
C. CPU stress > Stop at: cycle to **no limit**: the warning lists both TMUs' trip state and threshold
   (photo), then Cancel (or accept and run 1 min watching the phone).
D. Tests > **GPU tests** > Power up the GPU (warning, slide): "GPU powered" and vdd_g3d 950 mV. Note any
   failure line (photo). Then **Identification** (photo of the job slot features and the measured
   clock), **Job manager test** (every step's line, photo), **Fill pattern test** (the coloured square
   in the middle of the screen; PASS lines), **GPU stress** 1 min (Mpixel/s, GPU temperature).
E. Back out of GPU tests: the audit log says the GPU was powered down and vdd_g3d put back.
F. Optional, tier 3: GPU tests > GPU clock > 572 MHz (the iOS-proven level), then the job manager test
   again and a 1 min stress; then back to 260 MHz.
G. Tier 4 rehearsal, nothing written: Tools > Register peek / poke, address A0000000 (S-Boot's ECT),
   Write a word: arm, read the red screen (photo), **Cancel**. Logs > Audit log has "cancelled at the
   red warning"; Tests > Test history shows no action (cancelled before confirming).

Then the v2.0 steps that have not run yet:

1. The Dashboard shows, the tiles move; the status bar says STARBITE LAB. Keys and touch navigate as
   in v1.1 (hold Power = back).
2. Clocks > **Every thermal sensor**: TMU probes in the 30-45 °C range; the thermistor rows show
   values (battery near the room temperature) or "ADC clock gated" / "no conversion" (note which).
3. Tests > **Full health check** > Start. When it asks for **Start the M3 cores** (tier 3): first time,
   **cancel** (hold Power): the CPU step runs on the 4 A55s. Note every line of the result (photo).
4. Tests > **CPU stress**: Cores "the 4 A55 cores", 1 min, Start. Every A55 row shows batches/s and 0
   errors; the temperature chart rises. Hold Power: the result page shows at once.
5. Same with **all 8**: confirm the M3 sequence (type PMU, slide, hold Power 3 s). Note whether the 4
   M3 rows run, show "no check-in in 1 s", or FAULT (photo of the ESR / ELR).
6. Tests > **Thermal test**: the 4 A55 cores, 5 min, limit 85 °C. Note the peak, the rise per minute,
   and whether throttling is reported (and at what temperature).
7. Tests > **Memory stress**: 126 MiB, 1 pass: PASS and the bandwidth. **Storage test** (UFS, 64 MiB):
   MiB/s, IOPS, 0 mismatches.
8. Tests > **Battery tests**: Battery health (health %, cycles, temperature source). Then
   **Voltage sag and stability** (unplugged): the R per cycle. Then (charger in) **Charge test**, 2 min,
   watchdog feed off: does CHG state say "WATCHDOG EXPIRED"?
9. Tests > **Display stress** (30 s, hold Power to stop early) and **Touch grid** (every cell).
10. **First write, harmless**: Tests > Save report to the OS store: warning, Continue, slide. Then
    Starbite > OS store: the `lab-...txt` entry is there; open it > Verify the CRC: OK. On the Mac
    later: pull the store and read the report.
11. Storage > Partitions and LUs > **EFS** > Back up to the OS store (tier 1): a `bak-EFS-1` entry,
    "Verified". > Compare with a backup: identical. (No write to EFS.)
12. Tools > Settings editor: change the timeout, Save (slide), reboot to the menu and check it took.
    Set it back.
13. Tools > Armed mode: arm, then Register peek / poke: write a GPIO_CMGP data register you know is
    harmless (tier 1), Undo it. Note any fault / SError line in the audit log.
14. Logs > Audit log: every step above is listed.

Anything odd: reboot to TWRP and pull pmsg (`eng:`, `audit:`, `fence:`, `gpu:`, `SBENG1 W` and `CRASH`
lines) before starting another OS.

## 13. What Starbite Lab still cannot do, and why (v2.1)

- **Draw with shaders.** The GPU tests use clear-only fragment jobs and WRITE_VALUE jobs. Triangles,
  textures and compute need compiled shaders and tiler / vertex jobs: the iOS port drew a triangle
  from a blob Mesa's Bifrost compiler built offline; bringing that blob (and a compute job) in is the
  next step, not done. So the GPU's shader cores are powered and clocked but not exercised by ALU
  work: the stress loads the job manager, the tile write-back and the memory path, not the arithmetic.
- **Raise the CPU clocks.** Engineer OS reads the CPU clocks and detects throttling, but has no CPU DVFS
  page (ACPM domains 2 / 3 worked under iOS); the stress runs at what S-Boot left (~1050 MHz measured
  under iOS before DVFS).
- **Use interrupts.** Everything is polled with interrupts masked; the GPU's job IRQ is only checked as
  a pending bit at the GIC.
- **Prove the thermal trip.** The 115 °C trip is decoded from S-Boot's registers; nothing has ever
  tripped it, and Lab will not try to.
- **Write secure memory.** A tier 4 write to RKP / TIMA / the secure world is attempted and its fault
  reported; TrustZone denies the non-secure CPU, so the write cannot succeed. That is hardware, not
  policy.
- **Address memory above 64 GiB** (the MMU's 36-bit space): nothing of the S9 is there.
- **Recover a bad ECT by itself.** It backs the ECT up into the store; writing it back is a memory
  fill per word or a reboot (S-Boot reloads it, unverified), and there is no "restore memory from a
  backup" tool.
- **Restore a boot LU or the GPT if the phone no longer boots.** Lab's backups need a working Lab (or
  the Mac and Odin / Download mode with Samsung-signed images).
- **Know the battery's true capacity** beyond the fuel gauge's own model; the capacity estimate is
  mAh moved / SOC drop.
- **Talk to the modem, Wi-Fi, Bluetooth, cameras, audio, sensors hub, NFC or the fingerprint
  reader**: no drivers in libsb (Radio is a "coming" page).
- **Measure the display's frame rate or brightness curve** precisely: no vsync source is used.
- **Run anything from v2.1 on the phone yet**: every v2.1 feature is host-tested only (section 10).

## 14. Overclocking

Clocks > **Overclocking** (`engineer/oc.c`, tables parsed by `engineer/octab.c`). For every ACPM DVFS
domain (0 MIF, 1 INT, 2 A55, 3 M3, 4 G3D, 5 INTCAM, 6 FSYS0, 7 CAM, 8 DISP, 9 AUD, 10 IVA, 11 SCORE,
12 CP) it shows:

- the frequency steps from **S-Boot's ECT** (DRAM 0xA0000000, read in place, never written: the DVFS
  block's level lists) and **Samsung's caps** (the GEN block's `MINMAX_<domain>` rows, one per ASV table
  version; until this chip's version is read, the lowest cap over the rows is shown);
- **ACPM's voltage per step** from its fvmap in APM SRAM (first at the dtsi offset 0x6700, else a scan of
  the SRAM; a block is accepted only if its A55, M3 and G3D rates are the ECT's);
- the **current step** where a PLL can be read (A55, M3, G3D, MIF: PLL_MIF runs at twice the MIF step).

Clock tab rows after it moved: Real-time clock 0x67, Timer 0x68.

**Changing a step (step 2; master plan 1.2 rule 3 as amended 2026-11-08, this category only).** MIF, INT,
A55, M3 and G3D list their steps as rows; the other eight domains are read only (none of their blocks runs
in Engineer OS, so a step could not be tested). Tapping a step:

1. refuses anything that is not in both the ECT and the fvmap (`oct_allowed`; libsb's
   `sb_acpm_dvfs_listed` checks the list again; `sb_acpm_dvfs` refuses MIF / INT outright);
2. reads ACPM's step (FREQ_GET) and libsb's baseline (the step before Engineer OS's first request);
   refuses if that baseline is not a listed step (no safe way back);
3. G3D: powers the GPU first (the GPU page's tier 1 power-up);
4. confirms: tier 3 for A55 / M3 / G3D up to Samsung's cap, **tier 4 for MIF, INT and every above-cap step**;
5. arms the cluster 1 watchdog (40 s, `libsb/wdt.c`, fed from `ui_tick`) and **holds storage**
   (`sb_storage_hold`: every `sb_write` / `sb_write_store` returns `SB_W_HOLD`), then sends the request;
6. reads FREQ_GET back; a different answer reverts at once;
7. counts down 15 s: **Power keeps it**; Volume, Back or the timeout reverts;
8. offers the stability test: A55 / M3 CPU stress on that cluster for 60 s, G3D GPU stress for 60 s,
   MIF / INT memory patterns (64 MiB, 3 passes). A failure reverts the domain.

The watchdog and the storage hold stay on until every domain is back (Overclocking > Every domain back,
or each domain's "Back to the default step"). Reboot, boot-next and power off revert every domain first
(`sb_before_reset`). The green-screen freeze (NOTES.md 2026-10-30) came from asking ACPM for MIF 2093000 /
INT 667000 kHz; 667000 is not an INT step, which the list now makes impossible to send.

Not yet verified on the phone: that a watchdog reset fires while the memory bus is hung, ACPM's answer
for a cluster that is powered off (M3 between tests), and the ASV table version row.

**Self-test image** (`ENG_DEFS=-DOC_SELFTEST ENG_OUT=starbite/out/ocself sh starbite/engineer/build.sh`):
at start it logs every domain's table to pmsg (`oc <DOM>: ect N fv N init N cap kHz boot kHz now kHz rows
...`, then `kHz:uV` pairs six a line), then asks ACPM **FREQ_GET** for A55, M3 and G3D, then opens the
page. It requests no step.
