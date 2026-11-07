# Windows 11 on the big cores (master plan W2/WC2/WC3)

Started 2026-11-07, on the user's request: "make Windows 11 run on the big cores too and at their
maximum clock speed". Owner of the firmware side (MADT, PPTT, CPU bring-up): the win-usb-host
session (it owns the Windows ACPI tables, docs/WIN11-USB.md 4.1 and 10). The clocks belong to the
GPU session's `sbthermal`, the single ACPM owner (domain 3 = M3), coordinated through the main
session. **Status: B1 passed on the phone (2026-11-07): the M3s run from UEFI. B2 built, not run.**
Not for unattended boots: a CPU_ON that never returns needs a forced reset (the watchdog doesn't
rescue it).

## 1. What stands in the way

| # | Problem | Evidence |
|---|---|---|
| 1 | **The M3 cores may not run when started from UEFI.** CPU_ON (0xC4000003, target 0x100 + n) returns SUCCESS and FlexPMU marks the core up, but woa's UEFI never saw the core execute; a second CPU_ON to the same target never returns and needs a forced reset (the watchdog doesn't rescue it) | woa `PlatformBootManagerLib.c`, boot-v20..v36 notes |
| 2 | **Our XNU starts them**: six PMU writes before each CPU_ON (Samsung's `pmucal_lpm_init`, as the Linux S9+ port applies them): 0x1088 [31:24] = 0x8D, 0x2114/0x2180/0x21EC/0x2258/0x2384 [1:0] = 2 | `xnu/patches/0304-starlte-m3-cores.patch` |
| 3 | **Samsung's EL3 doesn't deliver PSCI `context_id`**, so Windows can't start cores itself; the firmware starts them into parking stubs and publishes mailboxes (ACPI parking protocol). The release BDS does this for the A55s only | woa notes boot-v20..v22 |
| 4 | **Affinity collision**: A55 core n MPIDR 0x81000n00, M3 core n 0x8000010n; A55 core 1 and M3 core 0 both have Aff1:Aff0 0x100. A MADT can't tell them apart | `docs/WIN11-24H2-A55.md` |
| 5 | **Features**: Windows boots on an A55 (ARMv8.2: LSE, PAN, RDM, DotProd, FP16, LRCPC, DPB); the M3s are ARMv8.0 without any of them. Windows will either refuse cores with fewer features than the boot CPU, or run v8.1/v8.2 instructions (kernel or apps told via IsProcessorFeaturePresent) on an M3, which is an undefined instruction there. XNU had to mask the common subset per core | NOTES 2026-10-28 (Linux sanity checks), 0304 |
| 6 | **EL2 isn't ours**: Samsung's uH owns EL2, so ID-register reads can't be trapped and sanitized for Windows | `docs/WIN11-24H2-A55.md` |
| 7 | The MADT's efficiency classes are backwards (M3 = 0, A55 = 1) and the release "P" module patches the MADT at ReadyToBoot | master plan 3.6, WIN11-USB.md 4.1 |
| 8 | Room: the release's fixed DXE slot (0x51000) is full; killing the release's SerialDxe (nothing consumes it without a TerminalDxe) frees it | WIN11-USB.md 10 |

## 2. The way through (stages)

| Stage | What | Done when | Risk |
|---|---|---|---|
| **B1** | **Do the M3s run from UEFI?** During dispatch: the six 0304 PMU writes, then exactly one CPU_ON per M3 core 1, 2, 3 into a witness stub (stores its MPIDR + a marker, cleans it to RAM, calls PSCI CPU_OFF). Stops at the first core that doesn't check in within 200 ms. MADT, PPTT, numproc untouched: invisible to Windows. Built (`M3Probe.c` in Star2LteSsdtDxe, `STARBITE_M3_PROBE=1`) | zone 0 shows `=M3 core n ... ARRIVED` for 1..3 and Windows boots as before | a CPU_ON that never returns hangs the boot before the logo (forced reset is fine there); user present |
| B2 | **Park the M3s for Windows**: start M3 1..3 into parking stubs (the A55 stub's design: baked mailbox address, poll + DC CIVAC, enter the OS with x0 = mailbox), mailboxes in reserved memory. Windows still 4 CPUs | the stubs report "parked" in zone 0 across a whole Windows session | parked cores spin (WFE-paced) the whole session |
| B3 | **Boot CPU = an M3** (WC3), so Windows sees only ARMv8.0 features and every core has at least those: early in UEFI, CPU_ON M3 core 1 and move the boot context onto it (registers, stack, VBAR/TTBR/TCR/MAIR/SCTLR/TPIDR, the GIC CPU interface and the generic timer for the UEFI tick), A55 core 0 goes into a parking stub like the other A55s | UEFI and winload run on M3 core 1; Windows boots with 4 CPUs (whichever are listed) | the hard one: a context switch of the firmware's own CPU |
| B4 | **7-core MADT + PPTT**: GICC entries A55 0..3 (MPIDR 0x000/0x100/0x200/0x300) and M3 1..3 (0x101/0x102/0x103; M3 0 left out for the collision), parking mailboxes for all six secondaries, efficiency classes A55 = 0 / M3 = 1, the boot CPU's entry first; PPTT with both clusters' caches (the M3 values read on an M3); BCD `numproc 7` (or removed). Has to coexist with the release "P" module's ReadyToBoot MADT patching: rewrite the table after it, or replace P's input | Task Manager shows 7 CPUs; a 10-minute all-core load runs; x64 apps run | feature mismatch (problem 5) if B3 isn't done first |
| B5 | **Clocks**: M3 DVFS through ACPM domain 3 with the temperature watchdog, in `sbthermal` (GPU session, single ACPM owner); A55 domain 2 likewise. Never MIF/INT | the M3s reach their top frequency under load and back off on temperature | heat; the watchdog first |

B2 and B3 can be explored in parallel once B1 answers. If B1 fails (the core never arrives even
with the PMU writes), the next step is reading FlexPMU's debug block and the PMU status words around
CPU_ON (woa's approach), still one CPU_ON per boot.

## 3. Stage B1 in detail

`tools/woa/modules/Star2LteSsdtDxe/M3Probe.c`, called first in Star2LteSsdtDxe's entry when built
with `STARBITE_M3_PROBE=1`:

1. the six PMU writes (read, modify, read back; logged `=M3 pmu <off> <before> -> <after>` on the
   first pass), **re-applied before every CPU_ON** as XNU and the Linux port do;
2. for M3 core 1, 2, 3: one reserved page below 4 GiB, the 17-instruction witness stub at +0x800
   (page address baked in, `MRS MPIDR` stored at +0x10, marker 0x4D334F4B at +0x08, DSB, DC CIVAC,
   DSB, `SMC` CPU_OFF 0x84000002, then WFE forever), the page cleaned to the point of coherency and
   the I-cache invalidated, then **one** CPU_ON (target 0x100 + n, entry = the stub, context 0);
3. poll the marker for 200 ms (clean + invalidate the line before each read); log
   `=M3 core n target 10n: CPU_ON rc=..., ARRIVED|no mark after .. ms, mpidr=...`;
4. stop at the first core that returns an error or doesn't arrive: never a second CPU_ON; 50 ms
   between cores so a core's CPU_OFF finishes before its sibling's CPU_ON (PSCI 0.1 has no
   AFFINITY_INFO to ask). The SMC helper declares x4..x17 clobbered (pre-SMCCC-1.1 firmware), as
   `shim/starlte/em.c` does. Reviewed (encodings assembled and compared with clang).

Image: `starbite-uefi-m3probe.bin` (sha256 87df200c…) (`SOLO=m3probe M3=1 tools/woa/build-win-usbhost.sh`): src38 +
Star2LteSsdtDxe with only the probe (no USB, no blobs, no PPTT fix), checked file by file.

**Phone test (user present):** boot it as a hidden Windows entry with nothing plugged. Expect the
Samsung logo, then Windows as usual (4 CPUs). If it stops on the Samsung logo for more than 30 s: a
CPU_ON hang (problem 1); force reset (fine before the Windows logo), boot src38. Afterwards read
zone 0 at the loader menu (`fastboot oem log dmesg`, before TWRP): the `=M3` lines answer B1.

**Result (2026-11-07, main session, user present, image 87df200c): B1 PASSED.** No hang; Windows
desktop as before. Zone 0: the PMU writes took (1088 `C7150000 -> 8D150000`, the five option
registers `00000001 -> 00000002`), then for each of M3 cores 1, 2, 3: `CPU_ON rc=0, ARRIVED after
1 ms, mpidr=8000010n`, and "cores 1..3 ran and powered off again". The XNU PMU sequence is what
woa's attempts lacked.

## 3.1 Stage B2: park the M3s for the whole Windows session

`M3Park` (in `M3Probe.c`, `STARBITE_M3_PARK=1`; never together with the probe) during dispatch,
for M3 cores 1, 2, 3:

1. one reserved page below 4 GiB (`EfiReservedMemoryType`, Windows leaves it alone) as the core's
   **ACPI parking-protocol mailbox** (ACPI 6.4 5.2.12.14: CpuId at +0, JumpAddress at +8, the OS's
   half to +0x7FF, the firmware's from +0x800);
2. the line `=M3 park core n mbox <page> up 00000000 s (hex)` in the zone-0 ring; its digits'
   address is found in the ring (it is never moved: the ring only appends);
3. the stub (`M3ParkStub.S`, 200 bytes, assembled by `gen-m3park-stub.py` into `M3ParkStub.h`;
   position independent, aligned accesses only because the MMU is off) copied to mailbox + 0x800
   with three literals: the mailbox, the digits' address, the UID (MADT GICC UIDs: M3 core n =
   4 + n); the page cleaned to the point of coherency, the I-cache invalidated;
4. the PMU defaults, one CPU_ON (target 0x100 + n, entry mailbox + 0x800), poll 200 ms for the
   stub's arrival mark (MPIDR at mailbox + 0xFF8); stop at the first core that doesn't park, as in B1.

The stub: arrival mark; timer event stream on (CNTKCTL_EL1 EVNTEN, EVNTI 15: a wake-up every
2^16 ticks of CNTVCT, ~2.5 ms at 26 MHz; CNTVCT because EL2 may trap the physical count);
then `WFE` forever. Every 1024 wake-ups (~2.6 s) it writes the seconds since parking
(CNTVCT delta / CNTFRQ) as 8 hex digits into its zone-0 line. After every wake-up it reads the
mailbox: JumpAddress != 0 and CpuId == its UID -> JumpAddress = 0, jump with x0 = the mailbox
(the parking protocol, for B4). In B2 the MADT is unchanged, so Windows never writes these
mailboxes: the cores just stay parked, asleep in WFE (no busy loop, so no heat).

**Evidence without a new reader:** after the Windows session, Start -> Restart into the loader menu,
`fastboot oem log dmesg`: the `=M3 park core n ... up XXXXXXXX s` lines hold how long each core
stayed alive (hex seconds, frozen at the reset). About the session length = parked all along;
00000000 = the core never woke from WFE (no event stream) or died early.

Image: `starbite-uefi-m3park.bin` (`SOLO=m3park M3PARK=1 tools/woa/build-win-usbhost.sh`): src38 +
Star2LteSsdtDxe with only the parking (no USB, no blobs, no PPTT); sha256 c813d8df…. Checked: the
module holds the reviewed 200-byte stub once, and neither the probe nor the PPTT code.

Risks: as B1 for each CPU_ON (one per core, PMU defaults first; B1 showed all three arrive in 1 ms).
New: three cores stay powered for the whole session, outside Windows' knowledge. They are asleep in
WFE with interrupts masked; Windows programs the GIC for its four CPUs only; restart is PSCI
SYSTEM_RESET, which resets them too. Unknown: whether Samsung's uH (EL2) traps WFE (HCR_EL2.TWE). The digits
would show it (00000000 or a stuck value), and the A55s are not involved. If Windows misbehaves (hangs, slowdowns, heat), force reset
is not needed: Start -> Restart, then boot src38.

## 4. Sources

`xnu/patches/0304-starlte-m3-cores.patch` (ours) for the PMU sequence and targets; woa's
`PlatformBootManagerLib.c` (BSD-2-Clause-Patent; read, not copied) for the parking protocol, the
EL3 context-id finding and the M3 CPU_ON history; the stub is written here from the ARMv8 encodings.
