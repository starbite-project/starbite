# Windows 11 on the big cores (master plan W2/WC2/WC3)

Started 2026-11-07, on the user's request: "make Windows 11 run on the big cores too and at their
maximum clock speed". Owner of the firmware side (MADT, PPTT, CPU bring-up): the win-usb-host
session (it owns the Windows ACPI tables, docs/WIN11-USB.md 4.1 and 10). The clocks belong to the
GPU session's `sbthermal`, the single ACPM owner (domain 3 = M3), coordinated through the main
session. **Status (2026-11-07): B1 passed (the M3s run from UEFI). B2 parked all three (uptime
showed 0: CNTFRQ_EL0 is 0 on this platform, see 3.2). Goal from the user: "actually 8 cores fully
in Windows". Path: B3a (built, not run) -> B3 -> B4 (7 cores) -> WC2 (8, research); section 5.**
Windows here is **23H2 (22631.7584)**, which runs on ARMv8.0. A 24H2/25H2 upgrade would end this
track: they need LSE atomics, which the M3s lack (docs/WIN11-24H2-A55.md).
Not for unattended boots: a CPU_ON that never returns needs a forced reset (the watchdog doesn't
rescue it).

**2026-11-07 night, ownership and a second path.** The MADT/PPTT/M3 firmware (M3*.c, Pptt*.c, pptt-patch.py,
this document, the M3/PPTT flags in build-win-usbhost.sh and gen-0024.py) moved to the "Windows 8 cores + x64
emulator" session (branch claude/laughing-pare-2ca469); the USB/SSDT parts stay with win-usb-host. Next to B3/B4 it
adds **Cores8** (docs/WIN11-24H2-A55.md, "All 8 cores"): Windows keeps booting on A55 0 and the M3s join as
parked secondaries (B2's dispatch-time parking, v1 mailboxes, MADT and DSDT _MAT updated at ExitBootServices).
It skips the boot-CPU hand-off and its shim problem, but it depends on Windows handling the A55/M3 feature
difference (test C3). If that fails, B3 v3 (b5ac1e78, not yet run) is the way around it.

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
3. the stub (`M3ParkStub.S`, 200 bytes, assembled by `gen-stubs.py` into `M3ParkStub.h`;
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

**Result (2026-11-07, main session, user present, image c813d8df): parked, uptime 0.** Windows ran
normally for several minutes and restarted cleanly (Basic Display that boot: the B images had no
`SBGPU`). Zone 0: PMU defaults as in B1, then for cores 1, 2, 3 `CPU_ON rc=0, PARKED after 1 ms,
mpidr=8000010n, digits found`, "3 of 3 M3 cores parked". But every core's line still read
`up 00000000 s`. Possible causes:
(a) CNTFRQ_EL0 is 0 on a freshly powered M3 (EL3 programs it per core; `udiv` by 0 gives 0 without
trapping), so a live core would keep writing 00000000. This one is likeliest: B2's uptime divided by
CNTFRQ.
(b) No event stream reaches the core, so WFE never returns.
(c) uH traps WFE (HCR_EL2.TWE) and doesn't resume the core.
(d) The core died right after its arrival mark.

## 3.2 Stage B2b: tell those apart

Same parking (`M3Park` v2, `STARBITE_M3_PARK=1`), with a diagnostic stub (`M3ParkStub.S`, 392
bytes) and one zone-0 line per core with hex fields, each 0 until the stub writes it:

`=M3 park core n mbox XXXXXXXX el E frq FFFFFFFF kctl KKKKKKKK spin SSSSSSSS wake WWWWWWWW t TTTTTTTT`

| Field | Written | Reading |
|---|---|---|
| `el` | at entry: CurrentEL >> 2 | 1 expected (UEFI is EL1 under uH); 0 = the core never got this far |
| `frq` | CNTFRQ_EL0 as read | 018CBA80 = 26 MHz, set; **00000000 = cause (a)** |
| `kctl` | CNTKCTL_EL1 read back after setting EVNTEN + EVNTI 15 | low byte F4 (or F7) = the event stream is configured |
| `spin` | phase A: 8 rounds of 2^26 `YIELD` iterations, no WFE (about a second in all), after each round | 00000008 = the core runs code; less = it died in phase A |
| `wake` | phase B: WFE wake-ups, every 256 | about 400 per second of session (5 min ≈ 0001D4C0) = event stream works; **00000000 with spin 8 = WFE never returns, (b) or (c)**; small = only SEVs from Windows |
| `t` | (CNTVCT - t0) >> 16 (2.52 ms units at 26 MHz), after each spin round and each wake update | grows with the session = the counter runs; with `wake` it dates the core's last sign of life |

What each outcome means for B2/B4:
- (a): the parking works. The stub (and Windows, later) must not rely on CNTFRQ; the firmware can set it per core (EL1 can't write CNTFRQ_EL0; ask EL3, or use 26 MHz as a constant).
- (b): the event stream is unavailable; use SEV from the A55 side, or a bounded `YIELD` spin with a long sleep instead.
- (c): WFE is not an option under uH; a `YIELD` spin it is, with its heat cost measured.
- (d): the parking itself needs work before B4.

Image `starbite-uefi-m3park2-gpu.bin`, sha256 edb78134… (`SOLO=m3park2 M3PARK=1 EXTRA_AML=<StarbiteGpu.aml>
SUFFIX=-gpu`): src38 + Star2LteSsdtDxe (parking only) + `SBGPU`, so Windows keeps its display driver
(no Basic Display fallback, no resolution flip). Same test as B2: user present, several minutes
of Windows, Start -> Restart into the loader menu, `fastboot oem log dmesg`. Risks: as B2. New: phase
A spins each M3 for about a second at boot (negligible heat).

**Not needed after all:** NOTES.md (2026-10-2x) has `CNTFRQ_EL0 = 0` on this firmware on *every*
core (S-Boot leaves it unset). TimerDxe gets 26 MHz from a patch, and the woa release patches winload
and ntoskrnl in memory at the handoff to use 26 MHz too. So B2's zeros were the divide (a), and the
cores were most likely alive. B2b (image edb78134…) would still tell (b)/(c)/(d) apart, but B4's
parking stub polls instead of using WFE (Windows takes the cores within seconds of kernel start),
so the answer doesn't change the design. B2b is optional; the 8-core path skips it.

**B2b result anyway (2026-11-07, image edb78134, a few minutes of Windows): PASSED.** Core 1: `el 1
frq 00000000 kctl 000000F4 spin 00000008 wake 00032300 t 00012B40`; cores 2 and 3 alike (wake
00030400 / 00030100, t 00012B29 / 00012B0C). CNTFRQ is 0 as expected. The event stream is
configured, and WFE returns about 1,060 times a second (more than the ~400/s stream alone, so
Windows' SEVs wake them too). All three lived 0x12B40 × 2.52 ms ≈ 193 s, the whole session. So
parked M3s can sleep in WFE through a Windows session; B4's stubs may use WFE + polling.

## 5. The way to 7 and 8 cores in Windows

Order and why: Windows must **boot on an M3** (B3) before the cores are mixed (B4). Booting on an
A55 would make the kernel and apps use v8.1/v8.2 instructions (LSE atomics and more) that an M3
can't execute.

### 5.1 B3a: borrow an M3 in the UEFI context (invisible to Windows)

`M3Run.c` + `M3RunStub.S` (`STARBITE_M3_RUN=1`). For each M3 core 1..3, during dispatch: A55 core
0 raises to TPL_HIGH_LEVEL, writes back its data cache, and hands its EL1 context (SCTLR, TCR,
TTBR0, MAIR, CPACR; a stack of its own) to the M3. The M3 turns its MMU and caches on with UEFI's page
tables and runs a C test:
- ID registers, CTR_EL0, CLIDR;
- SCTLR (M, C and I really on);
- CNTFRQ;
- GICD_ITARGETSR0 byte 0, its own GIC CPU interface bit, which is the MADT's "CPU Interface Number";
- coherency both ways (a pattern the A55 wrote *after* the write-back, so only in its cache, summed
  on the M3; the M3's inverted copy checked on the A55).

Then Done = 1 and CPU_OFF. The A55 waits up to 1 s, then logs everything. The stub's own vector table
catches any exception on the M3 (ESR/ELR/FAR logged, Done = 3, CPU_OFF), so a failed M3 never stays
inside UEFI code. This proves B3's mechanism with nothing left behind and answers what B3/B4 need:
interface numbers (expected 0x10/0x20/0x40 for M3 1/2/3 = interfaces 4+n), CTR line sizes
(DminLine/IminLine must be safe for Windows' cache maintenance on mixed cores), feature registers.

Image `starbite-uefi-m3run-gpu.bin`, sha256 efbdeaa6… (`SOLO=m3run M3RUN=1 EXTRA_AML=<StarbiteGpu.aml>
SUFFIX=-gpu`): src38 + Star2LteSsdtDxe (B3a only) + `SBGPU`. Checked: the test function compiled to
a leaf with no stack, calls or globals; stub 0x8E0 bytes (vectors at 0, entry at 0x800).

**v1 result (2026-11-07, efbdeaa6): failed safely.** Windows booted normally. Core 1: `CPU_ON rc=0,
done=0 after 1000 ms, el=0, mpidr=0`, so the stub never got its first store in. The PMU writes did
happen (silently; the binary has the loop right before the SMC). The difference from B1/B2, which
worked: they cleaned the stub page *by address to the point of coherency* (DC CIVAC). B3a cleaned by
set/way (WriteBackDataCache) and only to the point of unification (InvalidateInstructionCacheRange).
The M3, fetching with its MMU off, most likely saw a stale page. The A55's TCR (0x280803518: 4K, 40-bit
IPS, no HA/HD/TBI), MAIR and SCTLR (v8.1/v8.2 bits RES1 on v8.0) are fine for an ARMv8.0 core.
**v2** (`starbite-uefi-m3run2-gpu.bin`, 3d6c0158…):
- DC CIVAC by address over the stub page and the context (the coherency pattern stays dirty on
  purpose);
- the PMU writes logged as in B1;
- a **Stage** breadcrumb: 1 written first thing with the MMU off, 2 after MMU-on, 3 after the test.

B3 got the same fixes (image 702c1f22…, Stage 3 = claimed).

**v2 result (2026-11-07, 3d6c0158): PASSED.**
- PMU writes as in B1. Cores 1, 2, 3: `CPU_ON rc=0, done=1 stage=3 after 1 ms`.
- SCTLR 0x30C5183D on the M3 (M, C, I on: UEFI's page tables work there). Coherency A55->M3 and
  M3->A55 ok on all three.
- **GIC interface bits 0x20/0x40/0x80**: M3 core n = CPU interface 4 + n, as the MADT says.
- M3 ids: MIDR 0x531F0020, **CTR 0x84448004 (the same as the A55's)**, ISAR0 0x11120 (no LSE;
  A55 0x10211120), ISAR1 0 (A55 0x100001), PFR0 0x2222, MMFR0 0x101122, MMFR1 0 (no PAN, VHE,
  LOR, HPDS; A55 0x10212122), MMFR2 0, CLIDR 0xB200123 (L1 I+D, L2, L3).
- CNTFRQ 0.
- Windows booted normally afterwards.

`el=0 mpidr=0` was a reporting artefact. The stub writes them with the MMU off, so not through any
cache. The A55's polling held a clean, stale copy of that line, and the M3's first cacheable store
to the line (Stage) could be filled from that copy. Both stubs now record EL and MPIDR again right
after MMU-on.

### 5.2 B3: Windows boots on M3 core 1 (4 CPUs: M3 1 + A55 1..3)

At ExitBootServices (our own event, registered by Star2LteSsdtDxe), on A55 core 0:
1. Build the new MADT contents in a buffer (not yet in the table):
   - M3 core 1's GICC entry first (UID 5, MPIDR 0x101, interface 5, enabled, efficiency class 1);
   - A55 1..3 as the release's "P" left them (their parked addresses and mailboxes, enabled, class 0);
   - A55 0 and M3 0/2/3 disabled.
   The checksum is recomputed.
2. Save the full EL1 state: GPRs, SP_EL0/SP_EL1, SPSel, DAIF, NZCV, FPCR/FPSR, q0-q31, SCTLR, TCR,
   TTBR0/1, MAIR, AMAIR, VBAR, CPACR, CONTEXTIDR, TPIDR_EL0/EL1, TPIDRRO_EL0, CNTKCTL. Write back
   the data cache. CPU_ON M3 core 1 into a resume stub (reserved memory).
3. The M3 restores everything (VBAR = UEFI's this time). It copies the prepared MADT into the
   table (the commit point), sets Resumed, and returns from the callback into ExitBootServices and
   winload: from here winload and the kernel run on the M3.
4. A55 core 0 spins without its stack until Resumed, then PSCI CPU_OFF. **Fail-safe:** if Resumed
   doesn't come within 1 s, the A55 sets Cancel (the resume stub checks it first and powers off)
   and returns normally. The MADT was never touched, so Windows boots on the A55 as today. Either
   way, one line in zone 0.

Built: `M3Boot.c` + `M3BootStub.S` (`STARBITE_M3_BOOT=1`); image `starbite-uefi-m3boot-gpu.bin`,
sha256 27dd38b9… (with the B3a v2 fixes and EL/MPIDR recorded after MMU-on). Fallback
`starbite-uefi-m3bootsolo-gpu.bin` (cadc59d3…, `M3BOOT=2`): the same with M3 1 the only enabled
GICC, to split "Windows on an M3" from "M3 boot CPU + A55 secondaries" if B3 fails after the claim.

**B3 v1 result (2026-11-07, 27dd38b9): fail-safe worked, hand-off not taken.** Windows booted on the
A55 as usual (4 CPUs) and restarted cleanly.
- P's MADT, seen for the first time: UIDs 0..3 = A55 0..3 (interfaces 0..3, flags 1, MPIDR
  0..3, parking v0 for UID 0, v1 with mailboxes 0xBBC70000/71000/72000 for UIDs 1..3, efficiency
  class 1); UIDs 4..7 = M3 0..3 (interfaces 4..7, **flags 0 = disabled**, MPIDR 0x100..0x103, no
  mailboxes, class 0).
- PMU writes as in B3a.
- `CPU_ON rc=0, state=2` (the A55 gave up after 1 s), and `stage/el/mpidr = 0`. Those breadcrumbs
  are not evidence: they shared a cache line with State/Rc, which the A55 writes (Rc, right after
  CPU_ON), so its dirty copy written back over the M3's MMU-off stores.

The difference from B3a, which worked during dispatch: v1 ran the M3 from a separately allocated
page, and at ExitBootServices that page may not be executable in the active tables. Then the
first fetch after MMU-on faults, the vectors (same page) fault too, and the core hangs silently.
**v2:**
- The entry code and vectors are in this image's own .text (2 KiB-aligned vectors at RVA 0x1800,
  entry 0x2000), which the A55 is running from at that moment. The context pointer is in .data,
  cleaned to PoC.
- The breadcrumbs (DStage/DEl/DMpidr/DEsr/DElr/DFar) have their own cache line at context +0x380,
  which the A55 never writes after cleaning it.
- At ExitBootServices the A55 logs TTBR0/TCR/SCTLR/MAIR next to their values at arming, and the
  page-table entries of the entry code, the context and the stack.

Images: `starbite-uefi-m3boot-gpu.bin` 52c744d1…, solo `starbite-uefi-m3bootsolo-gpu.bin` 086f9a8c….

**B3 v2 result (2026-11-07, 52c744d1): not taken again, now with reliable evidence.**
- Translation tables at ExitBootServices: the same as at arming (ttbr0 0xBC7EF000, tcr, sctlr, mair
  all unchanged).
- The entry code is mapped as normal memory, executable (L3 desc 0xBBD4478F, attridx 3, pxn 0);
  the context and stack likewise.
- `CPU_ON rc=0`, but **stage=0**: the M3 never ran a single instruction of ours.

**The cause: the release's PSCI EL2 shim** (`Star2LtePsciEl2ShimStage0Proven`, NOTES 2026-10-26).
It installs during DXE: `=PSCI2 el=4 ... INSTALL1 ... hcr=80080002 ... post ver=0x10000`, where
`pre ver` was 0xFFFFFFFF. HCR_EL2.TSC=1, so from then on every SMC from EL1 traps into woa's EL2
payload, which acts as a PSCI 1.0. B1, B2 and B3a issued their CPU_ONs from our module's
dispatch, before the shim, so straight to Samsung's EL3 (PSCI 0.1), and they worked. At
ExitBootServices the shim takes the CPU_ON, returns 0, and the M3 never reaches our entry (likely
started in the shim's own EL2 code, built for the A55).

**v3:** M3 core 1 is CPU_ON'd at dispatch, only while PSCI_VERSION is still 0xFFFFFFFF, into a
parking loop (`M3ParkGo`) in reserved memory: MMU off, WFE with the event stream (as B2b), polling a
Go word. At ExitBootServices the A55 saves its context as before, then sets Go, cleans it and SEVs.
No SMC; the parked M3 jumps into the v2 entry in .text. On timeout: State 2 and Go back to 0, so the
core stays parked (reserved memory, harmless as in B2).
**New risk:** Windows' boot CPU then has no shim at EL2. Its PSCI calls (version/features at boot,
SYSTEM_RESET at restart) go to Samsung's EL3 directly, which is PSCI 0.1-like. A hang or bugcheck
after `now on mpidr=80000101` would point there; the next step would then be the shim's EL2 setup on
the M3.
Images: `starbite-uefi-m3boot-gpu.bin` b5ac1e78…, solo `starbite-uefi-m3bootsolo-gpu.bin` 620eace0…. (`SOLO=m3boot M3BOOT=1 EXTRA_AML=<StarbiteGpu.aml> SUFFIX=-gpu`). The live MADT is
logged at ExitBootServices (`=M3 boot MADT gicc uid=… if=… flags=… mpidr=… park v… eff=…`): the
first view of what P leaves. **Held until B3a passes** (its GIC interface bits and coherency
results gate this image).

`numproc 4` stays. Checks: Windows boots; Task Manager shows 4 CPUs; System Information / `wmic
cpu` shows the Mongoose (MIDR 0x531f0020) as CPU 0; a CPU-heavy run; Start -> Restart works.
Risks: a fault on the M3 after the commit point is a hang/bugcheck before the logo (forced reset fine,
boot src38). Unknowns: whether the release's EL2/PSCI shim or the handoff code cares which CPU runs
the handoff.

**Mixed features: B3 already tests them.** With M3 core 1 as the boot CPU, the kernel's global
feature set comes from the M3: no LSE, no PAN, no RDM/DotProd/FP16/LRCPC/DPB. IsProcessorFeaturePresent
reports that, and so does the x64 emulator's choice of atomics. A55 1..3 as secondaries are a strict
superset, so nothing Windows decides to use can be missing on any core. What B3 answers:
- whether 23H2 accepts secondaries whose ID registers differ from the boot CPU's (a refusal would be a
  bugcheck such as 0x3E MULTIPROCESSOR_CONFIGURATION_NOT_SUPPORTED);
- whether it handles PAN per CPU (present on the A55s only) or from the boot CPU alone.

If B3 fails that way, the solo image shows Windows itself runs on an M3. The way out would then need
EL2 (trapping ID registers is uH's).

### 5.3 B4: 7 cores (A55 0..3 + M3 1..3)

On top of B3:
- After resuming, the M3 CPU_ONs A55 core 0 and M3 cores 2 and 3 into **polling** parking stubs
  (B2's mailbox layout, no WFE), mailboxes in reserved memory.
- MADT: 7 enabled GICC entries, the boot CPU first. Distinct affinities: M3 1..3 = 0x101..0x103;
  A55 0..3 = 0x000/0x100/0x200/0x300 (A55 1 and M3 0 share 0x100, so M3 0 stays out).
  Parking protocol version 1 with the mailbox addresses. Efficiency class A55 = 0, M3 = 1.
- BCD `numproc 7` (main session, offline).
- PPTT: the release's, which already has leaves 0..7 in two clusters.
- Blobs: SBGPU, USB and WLAN carried, so the fast display and links stay.

Checks: Task Manager 7 CPUs; a 10-minute all-core load (temperatures watched: sbthermal owns M3
DVFS later, domain 3); x64 apps run (xtajit); restart.

### 5.4 WC2: the 8th core (research)

M3 core 0 has the same Aff1:Aff0 as A55 core 1 (0x100). The MADT holds only Aff3..Aff0, so Windows
can't tell them apart. Options to study later: does Windows compare the MT bit (MPIDR bit 24)? A
GICC entry with Aff2 or Aff3 set would need the core to report it, which only EL2 can fake (VMPIDR_EL2,
Samsung's uH).

## 4. Sources

`xnu/patches/0304-starlte-m3-cores.patch` (ours) for the PMU sequence and targets; woa's
`PlatformBootManagerLib.c` (BSD-2-Clause-Patent; read, not copied) for the parking protocol, the
EL3 context-id finding and the M3 CPU_ON history; the stub is written here from the ARMv8 encodings.

## 6. B3 v3 solo on the phone and v4 (2026-11-07 night, the 8-core session)

**B3 v3 solo (620eace0, user present): the hand-off worked, Windows did not start.** Zone 0: M3 1 parked at dispatch
(PSCI_VERSION FFFFFFFF, CPU_ON rc=0, 1 ms), the tables at EBS as armed, PSCI_VERSION 10000 (the shim), "now on
mpidr=80000101 (el 1), MADT committed (UID 5 first); winload continues here". Then nothing: the Windows logo, a hang,
the loader's watchdog reset ("HANG (WATCHDOG)"). No =UFSWRIT, no =UFSBUGCHECK. In every normal boot the woa wrapper
logs **=EBSOK** when ExitBootServices returns to winload (combo2's ring: =KEA, then =EBSOK); here it never came. So
the M3 hung **inside the rest of ExitBootServices**: the event handlers queued after ours and the DXE core's tail
(timer off, GIC off), all UEFI code, before winload ran a single instruction on the M3.

Suspect: UEFI drives the **physical** generic timer (Star2LtePkg.dsc: ArmGenericTimerPhyCounterLib; TimerDxe on
PPI 30), and stopping it at ExitBootServices writes CNTP_CTL_EL0 on the current CPU. The M3's EL2 controls
(CNTHCTL_EL2.EL1PCEN/EL1PCTEN, HCR_EL2) were set by Samsung's EL3 when it powered the core, not by S-Boot as on A55
core 0; if EL1 physical-timer access traps to an EL2 nobody set up, the core is gone without an EL1 exception.
Windows itself picks the virtual timer by default (ntoskrnl 0x14021F9F8: type 1 = CNTP, 3 = CNTHP, else CNTV) but
reads CNTPCT_EL0 in three places (0x14021F8A0, 0x14021FA84, 0x1404BB92C).

**v4** (M3Boot.c):
- the hand-off happens **after** the real ExitBootServices returned: gBS->ExitBootServices is wrapped at dispatch,
  so every handler and the DXE core's tail run on the A55, and the M3 only returns into the woa wrapper (=EBSOK
  must then appear) and winload. After the real call gBS is zeroed: StarbiteLog's mSbAfterEbs makes SbLog write the
  ring without boot services;
- solo builds probe M3 core 2 at dispatch (`=M3 tprobe core 2: ... crumb=N`): CNTVCT, CNTKCTL + CNTPCT, CNTP_CTL
  read, CNTP_CTL write, CNTV_CTL read, CNTV_CTL write, CNTP_CVAL read, a crumb after each (8 = all passed), plus the
  A55's own CNTVCT/CNTPCT at the same moment (a CNTVOFF difference shows as vct-vs-pct offsets).
Reading a v4 solo boot: crumb 8 and =EBSOK then a Windows hang = something later (kernel); crumb < 8 = that access
traps on the M3 (crumb 2: CNTPCT, 3: CNTP_CTL read, 4: CNTP_CTL write), and Windows needs the virtual timer path
everywhere, or EL2 set up on the M3.

**v4b** (after an independent review of v4, relayed by the main session):
- **winload's LSE latch.** winload.efi (md5 116bff41) carries the same outline-atomics helpers as ntoskrnl (init stub
  at RVA 0x3B10, latch word at RVA 0x210000: bit 31 initialised, bit 0 LSE) and initialises them on the A55 before
  ExitBootServices, so after the hand-off its interlocked calls would execute LSE on the M3. M3BootLseGuard, in the
  wrapper before the real ExitBootServices, finds every EfiLoaderCode image's init stub (MOVZ X16,#0x80000000; ADRP
  X17; STR W16,[X17]; MOVZ X16,#0; MRS X16,CurrentEL), sets its latch to 0x80000000 (initialised, no LSE: LL/SC,
  fine on both cores) and logs `=M3 boot lse: image ... latch +... old -> 80000000`. ntoskrnl and the drivers first
  run on the M3 and latch "no LSE" by themselves.
- x18 (the platform register) saved and restored with the rest of the context (Pad[0], +0x340).
- `=M3 state: daif isr vct gicc ctlr pmr hppir` on the M3 right after the commit.
Not changed: A55 core 0 still powers off through the shim (only that core uses the shim; the M3 has none); PSCI
from the M3 goes to Samsung's EL3 directly, so restart/shutdown may misbehave (not a boot blocker).
Images: solo4 a69941d4…, big3 17d5f1be…, big4 7c3a48a9….

**solo4 (a69941d4) on the phone:** one step further. Timer probe on M3 core 2: crumb 8 (every CNTV/CNTP access fine at
EL1 on an M3; vct-pct = -8), so the physical-timer theory is out. The LSE guard fired on winload ("image BB302000
size 211000 stub +22400 latch +1AF000: 80000001 -> 80000000": a real latent bug, now neutralised). ExitBootServices
returned Success on the A55, the hand-off claimed, "=M3 state: daif=2C0 isr=0 vct=261253EF gicc ctlr=1 pmr=F0
hppir=3FF" printed on the M3, then nothing: no =EBSOK, no exception marker. The woa wrapper calls our wrapper at
BdsDxe +0xAAF8 and goes almost straight to its =EBSOK print (+0xAB34; the print appends to pmsg while the index is
below 0x3F00, and the ring wasn't full), so the M3 dies between our state line and that print, or the print never
reaches memory.

**v5** (solo5): the A55 stays alive after the claim for 15 s as the M3's watcher (own stack page, reserved) and
rewrites one zone-0 line about 5 times a second:
`=M3 watch beat <A55 count> prog <M3 progress: 1 leaving M3BootAfterEbs, 2 leaving our wrapper> catch <exceptions on
the M3 after the claim> esr elr far <the first one's> pmsg <pmsg write index>`; then CPU_OFF as before. The M3 gets
M3BootCatchVectors after the claim (records ESR/ELR/FAR, then waits), and the context page is reserved memory now.
Image starbite-uefi-solo5-gpu.bin 4b47f25e….

**solo5 (4b47f25e):** `=M3 watch beat 0000004C prog 02 catch 00 ... pmsg 000031DD`: the A55 lived the full 15 s, the
M3 left our wrapper (prog 2) without any exception, and the pmsg index never moved after "=M3 state" (0x31DD = right
after that line). So the M3 returned into the woa hook (BdsDxe +0x9CBC, the call at +0xAAF8) and never reached its
=EBSOK print (+0xAB34), with no exception at EL1 (IRQ/FIQ are masked there, so an SGI can't sample its PC). Only
BdsDxe replaces gBS->ExitBootServices (its store at +0x428C, early in BDS); ours was installed at dispatch, i.e.
INSIDE woa's.

**v6** (solo6): our wrapper is installed at ReadyToBoot, after BdsDxe's hook, so it is the outermost: winload -> ours
-> woa's -> the real one. woa's post-EBS code (=EBSOK) runs on the A55 before the hand-off; the M3 returns straight
into winload. The wrapper logs its caller (`=M3 boot v6: ExitBootServices wrapper called from ...`: winload's
address expected, 0x90F4xxxx). Watch line as in v5. Image starbite-uefi-solo6-gpu.bin 0be43045….

**Correction (independent review relayed by the main session, checked here): =EBSOK was printed; the pmsg copies
hid it.** SbRingWrite advances the header's start (W[1]) and size (W[2]); the woa BDS print advances only W[2]; the
loader appends at W[1] after the reset. So everything BDS wrote after our last line (two `locp` trace lines and
"\n=EBSOK ra=<16 hex>") was overwritten by the loader's own lines, and ramoops shows the region [W[1], W[2]) as a
stale prefix at the top of the file: b3solo 91 bytes (2 trace lines + 27), solo4 27, solo5 27, exactly
`\n=EBSOK ra=` + 16 digits. The solo5 watcher read W[2] without invalidating the A55's copy, so its frozen value
proves nothing. **So the M3 got back into winload in v3..v5; it hangs later, in winload or the early kernel.**
Suspects: (a) the A55's CPU_OFF through the shim to Samsung's EL3 (woa's PlatformBootManagerLib notes EL3 hanging the
platform after a CPU_OFF from a core it started; a WFI park was stable); (b) Windows' early PSCI calls from the M3,
which go straight to EL3 (no shim there).

**v7** (solo7, on top of v6): the A55 never calls CPU_OFF: after its 15 s watch it parks in a WFI loop in its reserved
page with the MMU and caches off. The M3 calls PSCI_VERSION once after "=M3 state" (`=M3 psci: ...` before and after:
a missing second line = EL3 doesn't answer the M3). The watcher invalidates the pmsg header before reading it.
WriteBackDataCache (ASSERT(FALSE)/no-op in edk2 on ARM) is gone. The watch lasts 60 s (solo6 below). Image
starbite-uefi-solo7-gpu.bin 71e88218… (an earlier 15 s build, 95402dec…, was never run).
The loader's pmsg append fix (append at W[2] while unwrapped) is the main session's.

**solo6 (0be43045):** the order works as designed: "ReadyToBoot: ExitBootServices BBD27CBC wrapped by ours", "wrapper
called from 90F47638" (winload), the LSE latch 80000001 -> 80000000, =EBSOK now printed on the A55, the hand-off,
"=M3 state", watch `beat 4C prog 02 catch 00`. No =UFS line, no bugcheck: the M3 is in winload/ntoskrnl and hangs
there, with no EL1 exception during the 15 s watch; the A55 then did CPU_OFF (removed in v7).

**solo7 (95402dec, the 15 s build):** `=M3 psci: returned FFFFFFFF`: Samsung's EL3 answers the M3's PSCI_VERSION with
NOT_SUPPORTED (A55 0 sees the shim's 10000). Watch `beat 4C prog 02 catch 00 pmsg 34C5`, =EBSOK on the A55, no CPU_OFF
this time, still no =UFS. So Windows on the M3 finds no PSCI 0.2+ (the HAL makes about a dozen PSCI calls early);
the prime suspect now.

**v8** (solo8): the M3 gets the woa shim's EL2 payload too. The shim installs it with Samsung SiP SMC 0xC2000400
(x1 = base + 0x1800, x2 0x2000, x3 1, x4 base, x5 0x8000; EL3 runs it at EL2 on the calling core: VBAR_EL2, HCR_EL2.TSC,
back with SMC 0xC2000401). v8 finds the already loaded payload by its header (base moves between boots: BBC10000,
BB6E0000) on the A55 before ExitBootServices (`=M3 el2: woa PSCI payload at ...`), and on the M3 right after "=M3
state" makes the same SMC (`=M3 el2: SMC C2000400 ...`, then `rc= payload el= hcr= status=`: status 0 = installed),
then the PSCI_VERSION probe: 10000 = the M3 has PSCI 1.0 like the A55. Image starbite-uefi-solo8-gpu.bin 4764ed60….

**solo8 (4764ed60):** payload found at BB6E0000, but the SMC C2000400 from the M3 returned -1 (payload never ran:
el 0); PSCI_VERSION still FFFFFFFF. Samsung's EL3 installs that EL2 payload once (on A55 core 0, by the shim) and
refuses a second core. No way to EL2 on the M3 is known (a CPU_ON forwarded through the shim never reached our code in
B3 v1/v2).

**v9** (solo9): Windows on the M3 gets no PSCI at all. ntoskrnl issues PSCI through one generic SMC wrapper
(0x1402000A0; the other SMCs are SMCCC_ARCH_WORKAROUND_1 0x80008000 in exception stubs) and decides from the FADT's
ARM_BOOT_ARCH (offset 129: bit 0 PSCI_COMPLIANT, bit 1 PSCI_USE_HVC; the release has 0x1). v9 sets it to 0 at the
commit point on the M3 (`=M3 fadt: ARM_BOOT_ARCH 1 -> 0`): secondaries by the ACPI parking protocol (the v1 mailboxes
Windows already uses for the A55s and Cores8's M3s), restart by UEFI ResetSystem. The refused C2000400 attempt is
gone; the PSCI_VERSION probe line stays. Image starbite-uefi-solo9-gpu.bin 90f6c304….

**v10** (solo10, `M3PSCIEMU=1`, the fallback if v9 still hangs): Windows on the M3 keeps PSCI, answered like the
woa shim answers it. Before the real ExitBootServices the A55 finds ntoskrnl (a PE in EfiLoaderCode/LoaderData whose
export directory names "ntoskrnl.exe"; the memory map into a static buffer so winload's map key stays valid), finds
its one generic SMC wrapper (SUB SP,#0x10; STR X7; SMC #0; LDR X7; STR X0,[X7]; RVA 0x2000A0 in 22621.7582, unique),
copies M3PsciEmu.S (140 bytes: PSCI_VERSION -> 0x10000, PSCI_FEATURES -> 0 for the seven functions the shim lists,
-1 otherwise, everything else to EL3) into the zero slack after .text's VirtualSize (RVA 0x5D7D10, .text's last
page: non-paged, executable), and replaces the SMC with a branch there; the emulator's last instruction branches
back after the SMC. Checked on the pulled ntoskrnl mapped by RVA: SMC +0x2000A8 -> B 0x5D7D10, exit B 0x2000AC.
The FADT stays PSCI-compliant in v10. Log `=M3 psciemu: ntoskrnl ... SMC wrapper +2000A8 -> emulator +5D7D10`.
Image starbite-uefi-solo10-gpu.bin 1d195086…. big3/big4 rebuilt on v9: b47427bb… / bc8a9afa….

**solo9 (90f6c304):** still a hang and a watchdog reset at ~2 min (watch `beat 12D prog 02 catch 00`, PSCI_VERSION
FFFFFFFF, no =UFS). The log read `=M3 fadt: ARM_BOOT_ARCH 0 -> 0`: the live FADT already had 0 there, although the
release's platform table file has 0x1 at offset 129 (FACP rev 6, 276 bytes). So the FADT Windows gets is not that
static table byte for byte (something rewrites or replaces it before ExitBootServices), and the no-PSCI change was
a no-op. Open firmware-level question: which module publishes the live FADT and with what flags.

Status 2026-11-08: the big-core hand-off reaches Windows' loader on the M3, which then hangs silently. Further
work on this path is paused pending the user's decision (see the session's last report).

