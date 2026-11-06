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

Status (2026-11-06, v2.0): **built and checked on the host only** (unit tests, the reproducible build,
the relocation test, the keys-only tour of every page, the Lab tour that starts and stops every
test). Nothing new in v2 has run on the phone yet; v1.1 booted on the phone (libsb, touch). Section
11 has the status of each page, section 12 the phone checklist, section 10 what is unverified.

## 1. Layout

| Path | What |
|---|---|
| `starbite/libsb/` | the core: `crt0.S` (header, entry, self-relocation, vectors, PSCI core stub, **worker entry and worker vectors**), `reloc.c`, `mmu.c`, `sys.c`, `log.c`, `fdt.c`, `acpm.c` (S2MPS18; **gated `sb_pmic_write`**), `hsi2c.c`, `touch.c`, `max77705.c` (**gated `sb_max_write`**), `tmu.c` (every probe, the trip), **`adc.c`** (CMGP ADC, thermistors), `ufs.c` (**LU reads, fenced WRITE(10)**), `sd.c` (**fenced CMD24/25**), **`blkio.c`**, `store.c` (**append-only writer, settings_save, remove / rename**), `catalog.c`, **`fence.c`** (the tier fence and the authorisation gate), **`smp.c`** (the other 7 cores as workers), `crash.c`, `sb7.c` + `assets.S`, sbui (`sbui*.c`) |
| `starbite/engineer/` | the application: `main.c`, `tabs.c`, `pages_em.c` (em.c's pages), `pages_sb.c`, `dash.c`, `tests.c` (history), **`armed.c`** (armed mode, the warning screen and confirmations, backups, restores, gated writes, undo), **`policy.c`** (the tiers, pure), **`lab.c`** (the Lab tests), **`kern.c`** (the stress kernels, ARMv8.0), **`util.c`** (the utilities), `build.sh`, `preview/*.png` |
| `starbite/host/` | the host preview (`host.c` fake hardware, `preview.sh`, the keys-only `tour`, the **`labtour`**), `selfcheck.sh` |
| `starbite/tools/` | `mkassets.py`, `check-image.py`, `reloc-test.c`, `unit-test.c` |
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
x all 32 step subsets agrees with the tier rule); **the drivers** (`ufs_write_blocks` returns -6 and
`sd_write` -9 outside the fence before touching a register; the general UFS path still accepts only
4 read opcodes); **the stress kernels** (deterministic references, every core's batch matches, an
injected corruption and a wrong reference are caught, the worker loop publishes and stops); **the
thermistor tables** (battery: ADC 1950 = 26.2 °C, S-Boot's own reading; clamps; monotonic; AP and
PA tables); the key state machine.

Output: `starbite/out/engineer.img` (4407296 bytes, 575 relocations at v2.0; sha256
`da25de2c000491d234eb2662d3a3f7e387892e287a5be9bfe88a1e1d7c9736a6`).

### 2.1 Host self-checks (no phone)

```sh
sh starbite/host/selfcheck.sh        # build --repro + the keys-only tour + the Lab tour
sh starbite/host/preview.sh tour     # every row of every tab with Volume and Power (82 pages)
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
appended through the fence) and **an unconfirmed write refused** (Write fence page). On the host the
other cores are threads running the same kernels, the store and block devices are fakes behind the
real fence and gate, and the fake clock follows real time while the CPU is busy.

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
unchanged. The catalog's version string (`tools/store/catalog.toml`, outside this change) still says
"Engineer OS 1 · read only · dry-run writes" and should become something like "Starbite Lab 2 ·
tiered writes · stress tests". Flash with the usual store tools (pull the live store first, `--import`
it: backups and reports the Lab appended to the store are ordinary entries and are kept that way).

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
| Tests | **STARBITE LAB**: Full health check, CPU stress, Memory stress, Thermal test, Battery tests, Storage test, Display stress, Touch grid. **QUICK TESTS** (v1): Core bring-up test, Memory test, Memory bandwidth, Display test, Display panel, Brightness, Touch screen, Keys, Speed test. **RESULTS**: Test history, Save report to the OS store |
| Power | Battery and charger, Battery graph, **Battery tests**, PMIC regulators, PMIC register dump, Touch rails (tier 1, real), **PMIC and charger writes**, Reboot and power |
| Clocks | CPU, Clocks, Temperatures, **Every thermal sensor**, GPU (Mali-G72), Real-time clock, Timer |
| Storage | UFS host controller, UFS device and health, Partitions, GPT viewer, **Partitions and LUs**, Sector viewer (**any medium / LU, edit**), Speed test, **Storage test**, microSD card (**power and clock it up**), Card store at boot (**real settings write**) |
| Starbite | **OS store** (inspect, verify, copy, rename, remove), Catalog, Settings record, **Settings editor**, Start an OS, Write fence, Apple, iOS boot options, Android 17, Linux, Windows 11 |
| Logs | Engineer OS log, Boot log (pmsg), Loader handoff, Test history, Audit log, Retained records, Kernel command lines, Registers |
| Tools | **Armed mode** (arm, dry-run switch, undo, the tiers), **Register peek / poke**, **Memory dump / fill**, **PMIC and charger writes**, **Partitions and LUs**, **OS store**, **Settings editor**, **Save report**, Exit to the boot menu, **Reboot to** (menu, Engineer OS, any OS, Starbite Recovery, rescue, TWRP, Odin, power off), Radio / Installer ("coming"), About |

### 6.1 The Lab tests

Every test: a setup page (Power cycles an option), a live page (rebuilt 4 times a second), a result
page; the result goes to the test history (pmsg) and into the session report.

| Test | What it does | Live | Stops on | Result |
|---|---|---|---|---|
| **CPU stress** | the selected cores (all 8, the 4 A55, the 4 M3 + cpu0, cpu0) run integer (64-bit mul / mulh / div chains), FP (double Horner, sqrt, div), NEON (u32 MLA, f32 FMA lanes) and cache (512 KiB per core, address hash) kernels; every batch's result is compared with a reference the boot core computed | temperature and work-rate charts, per-core batches/s, totals, errors, faults, A55 / M3 MHz | Power, the limit (default 85 °C, 70..95), the time (30 s..60 min, or never), a worker fault | PASS = no error, no fault, cores ran |
| **Memory stress** | the free window at 0xA0100000 (16 / 64 / 126 MiB, non-cacheable): zeros, ones, both checkerboards, address and its inverse, walking ones and zeros, random; write all then verify, 1 MiB at a time; a bandwidth pass (write, read, copy) per pass | pattern, progress, errors, first bad address with read / expected, bandwidth chart | Power, the pass count (1, 3, 10, never) | bad words, first bad address, MiB/s |
| **Display stress** | 10 s each: full-screen colour fills, scrolling bars, an 80 px checkerboard flip, as fast as the CPU draws (no GPU) | frame counter | Power, Bixby | fps per mode, worst frame time, the user's verdict |
| **Touch grid** | 6 x 12 cells over the panel; touched cells turn green | count | all touched, Power | cells, the first missed one |
| **Storage test** | UFS SYSTEM (or the card from block 0): N MiB sequential read timed, the same range again with every MiB's CRC32 compared, then N random 4 KiB reads; read only | MiB/s chart, IOPS, errors, mismatches | Power | MiB/s, IOPS, average / worst latency, mismatches |
| **Thermal test** | 10 s idle baseline, the CPU-stress load on the selected cores for 30 s..60 min, a 60 s cool-down; every TMU probe, the thermistors and the clocks each second | hottest chart, work chart, every sensor, throttling state | Power, the limit, a fault | start / peak / rise °C/min / cool-down; throttling (the A55 / M3 clock or the work rate 15 % below the first 10 s of load for 3 s) with time and temperature; a 1 Hz log saved with the report |
| **Battery tests** | health (every fuel-gauge value, live); **discharge under load** (all cores + full white, cores, screen, idle; 2..30 min): coulomb counting, average mA / mW, capacity estimate = mAh / SOC drop, runtime at that load; **voltage sag and stability**: 5 x (10 s idle, 10 s full load), internal resistance per cycle; **charge test** (charger in): average current, time to full, watchdog expiry, optional watchdog feed (tier 1); capacity summary | V and mA charts, SOC, mAh moved, battery temperature | Power, 3.45 V, 10 %, battery 45 °C, the CPU limit | per test, PASS rules below |
| **Every thermal sensor** | TMU_TOP probes 0..7, TMU_SUB 0..2, the decoded hardware trip, six thermistors on the CMGP ADC, the fuel gauge's temperature register | live | - | - |
| **Full health check** | 15 checks in about 30 s: timer + RTC running, PMIC (ACPM), touch controller, battery and gauge (present, model, voltage, health), battery temperature, TMUs, CPU (every core 5 s, checked), temperature under load, 32 MiB pattern test, bandwidth, UFS + both GPT headers' CRCs, UFS read + verify, OS store + catalog + engineer entry, microSD, display | a checklist | Power (the rest: "not run") | pass / warn / fail per check |

Battery PASS rules: discharge: samples taken and the voltage stayed ≥ 3.3 V; sag: 5 cycles, average
internal resistance < 300 mΩ, minimum ≥ 3.3 V; charge: average > 50 mA and no watchdog expiry.

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
| 1 | warning screen + **held** confirmation (slide, or hold Volume Up 1.5 s) | OS partitions (SYSTEM, VENDOR, ODM, A16DATA, WINESP, WINDOWS, USERDATA outside the live store, CACHE, OMR, HIDDEN, CP_DEBUG), the microSD card, store appends (backups, reports), the settings record; known peripheral registers (pin controllers, the touch / MAX77705 I2C buses, the ADC, DECON, DSIM); free DRAM, the framebuffer, pmsg; the touch and card rails at the kernel's values; the RTC; the charger watchdog feed; reads of unknown registers (once per block) |
| 2 | + the target's name **typed** on the keypad | BOOT, RECOVERY, UP_PARAM, RADIO, partitions Starbite does not know, the live OS store (raw writes, rename / remove), erasing a whole partition; peripheral CMU gates (PERIS, PERIC0/1, FSYS0/1, APM, CMGP, DPU, AUD, the card gate in CMU_TOP), the UFS host and PHY, the card controller, the watchdog, the GIC, the always-on GPIOs (the keys), SYSREG_CMGP, any unknown device address; the UFS DMA window; S-Boot's FDT |
| 3 | + a **second confirmation** (hold Power 3 s), and for storage **a verified backup first** | EFS, CPEFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA0-2, DTB, DTBO, the GPT/PIT head and tail, **boot LUs 1..7**; **PMU** (power, reset, DVFS, CPU power-up, incl. the M3 sequence), **CMU_TOP / CORE / BUSC (MIF, INT)**, **MIF (CMU_MIF, DREX/DMC, DDR PHY)**, **CPU clusters (CMU_CPUCL0/1, DVFS)**, **CMU_G3D**, the ACPM / APM mailbox and APM SRAM, **CHIPID / fuses**, the **TMU** (S-Boot's 115 °C trip), every PMIC regulator / BUCK / TOP register or value other than the rails above, every MAX77705 register other than the watchdog feed, the secure-camera and modem carve-outs (may raise an SError) |
| impossible | none can unlock it | the unmapped secure world (0xAF800000..0xBFFFFFFF: the access faults), **S-Boot's ECT** (0xA0000000..0xA00FFFFF: the next OS reads it), **the running Engineer OS** (0x92000000..0x9FFFFFFF: code, stack, screen buffers), MAX77705 CHG_INT (clear-on-read status) |

**The flow** (`armed.c lab_confirm`), for every write:
1. the **warning screen**: the tier, what happens (one line per step), the target, now / after, how
   to undo it, the backup, which confirmations follow; Continue or Cancel (hold Power = cancel);
2. tier 2 and 3: **type the target's name** on the keypad (letters, digits, `_`; Volume moves, Power
   types, hold Power cancels);
3. the **held confirmation**: a sheet with what / target / now / after / backup / undo / tier and a
   slider (drag it, or hold Volume Up 1.5 s);
4. tier 3: the **second confirmation**, hold Power 3 s (red ring; Bixby cancels);
5. tier 3 storage: **`lab_backup`** copies the target range into the OS store (UFS, else the card's)
   as `bak-<NAME>-<n>`: every chunk read back and compared, the whole entry CRC'd again, then the
   source read once more and compared with the backup; the dialog and the audit log say where it went;
6. the write runs with the **gate** open on the authorisation; the write is read back.
Below 20 % battery without a charger every write is refused; if the gauge does not answer the user
is asked.

**The authorisation** (`fence.c struct sb_auth`): space (UFS / SD / memory / PMIC / MAX77705), LU,
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
does), the RAM test window during memory tests, the cache-maintenance of the worker protocol.

Only after the user confirmed it on the phone: every block write (UFS any LU, the card, the store,
the settings record), every register / memory write outside the above, every PMIC and MAX77705
write, the M3 PMU sequence, the card's clock gates and rails, the touch rails.

## 10. Untested and unsure (read before the first boot of v2)

Nothing new in v2 has run on the phone. The risky parts, in the order to try them:
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
  the INT_ST bit 0 / DAT bit 31 completion and the channel numbers (battery 4, charger 3, USB 1, AP 0,
  Wi-Fi 2, PA 10, from the vendor DT) are unverified. The gate 0x14202018 (ADC_CMGP PCLK_S0) is only
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

## 11. Status per page and test (v2.0)

"HW" = seen working on the phone; "host" = built, drawn and navigated in the host preview and the
tours, not yet looked at on the phone; "host+lab" = also started and stopped by the Lab tour.

| Page / test | Status | Writes |
|---|---|---|
| Start-up, relocation, MMU, crash vectors | **HW** (v1.1) | - |
| Touch input | **HW** (v1.1) | - |
| Keys-only navigation, TABS rows | host (tour: 82 pages) | - |
| Dashboard, device, SoC, memory map, reset, battery graph | host | - |
| Full health check | host+lab | RAM test window; M3 PMU sequence (tier 3, asked) |
| CPU stress | host+lab | PSCI; M3 PMU sequence (tier 3, asked) |
| Memory stress | host+lab | the RAM test window |
| Thermal test, Every thermal sensor | host+lab | ADC conversions |
| Battery tests (health, discharge, sag, charge) | host+lab (discharge, sag) | charger watchdog feed (tier 1, opt-in) |
| Storage test | host+lab | - (reads only) |
| Display stress | host+lab | framebuffer |
| Touch grid | host+lab | - |
| Save report to the OS store | host+lab (tier 1 write end to end) | store append (tier 1) |
| Quick tests (core bring-up, memory, bandwidth, display, panel, brightness, touch, keys, speed) | host (v1 code) | as v1 |
| Test history | host | pmsg |
| Battery and charger, PMIC regulators / dump | host | - |
| Touch rails | host | PMIC LDO35 / LDO43 (tier 1) |
| PMIC and charger writes | host | any PMIC / MAX77705 register (tier 1..3) |
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
| Register peek / poke, Memory dump / fill | host | any address (tier 1..3) |
| Logs | host | - |

## 12. Phone checklist (Starbite Lab v2.0)

Start it from the boot menu's Lab row / dock (or `tools/store/boot-next.sh engineer`). Only the
phone's buttons and screen are needed; tick what works, note what does not. Charge the phone above
50 % first. **Steps 1-9 write nothing to storage.**

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

Anything odd: reboot to TWRP and pull pmsg (`eng:`, `audit:`, `fence:` and `CRASH` lines) before
starting another OS.
