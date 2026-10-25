# Project Starbite — iOS (XNU) on Galaxy S9 SM-G960F (starlte, Exynos 9810)

Working notes. The original brief is in `~/Downloads/S9 Exynos 9810 — HackiOS (iOS) Project Brief.md`;
this file records what was actually found, and corrections to the brief.

## Status

- 2026-10-25: reference check done (below).
- 2026-10-25: **milestone 1 reached** — `shim/starlte` probe boots from BOOT, paints the panel, prints the
  handoff and reboots itself into Download mode (on two G960F units). Next: XNU loader stage.
- 2026-10-25: milestone 2 prepared, not built or flashed: STARLTE kernel config on top of the herolte series
  (`xnu/patches/0300-starlte-board-target.patch`, all 161 herolte patches apply on xnu-8796.141.3),
  `shim/starlte/xnu_boot.*` loader (XNU window 0x92000000..0xaf800000, log page 0x91fff000, failure ->
  Download mode), `tools/mkadt_starlte.py` (CPU0 only, GIC 0x10101000/0x10102000, 26 MHz, fbcon).
  S7 keys/touch/APM-PMIC/GPU/fuel-gauge code is compiled out for STARLTE; first boot is expected to end at
  "waiting for root device". Kernel build needs macOS + Xcode 15 (herolte CI on GitHub macos-14 runners).
- 2026-10-25: **milestone 2 reached** — XNU 8796 (STARLTE, fork starbite-dev/ios-a5xelte branch
  `starlte`, CI run 40191183432, kernel 17 MB) boots on the S9 from BOOT via shim/starlte loader
  (`tools/make-xnu-boot.sh`, flashed BOOT only). Photographed: CPU0 mpidr 0x0 EL1, SGI OK; timer test 20 ms
  sleep woke after 21019 us (26 MHz timebase ok); AppleCLCD 1440x2960 at 0xcc000000; zones/scheduler/BSD up;
  AMFI + Sandbox stand-ins, HFS registered; Skywalk self-tests passed; ends at `Waiting on <dict>
  IOProviderClass IOMedia ... Apple_HFS` (root wait, as expected without storage).
- 2026-10-25: **milestone 3 reached — read-only UFS from XNU.** `ufsprobe=1` on STARLTE (herolte_ufs.c with
  vendor HCI at +0x1100, one 4 KiB PRDT entry per READ(10), writes pinned off). Host after S-Boot:
  cap 0x1303ff0f ver 0x210 hcs 0x10f **hce 0x0** is 0 utrlba 0xef002000 dbr 0 rsr 1, txprdt/rxprdt 0,
  nexus 0x2 — HCE reads 0 but the host works (check relaxed for STARLTE). Device is asleep after S-Boot:
  W-LUN TEST UNIT READY -> ocs 0x7, CHECK CONDITION, sense 2/04/02; START STOP UNIT (ACTIVE) 0; LUN 0-3 TUR 0;
  READ(10) LBA1 = "EFI PART", GPT 128x128 at LBA 2: 25 partitions, p10 BOOT 19458+14080, p25 USERDATA
  1698432+13917563 (54365 MiB) (4 KiB blocks). Console wraps to the top of the screen when full.
- 2026-10-25: iOS 16.7.16 (20H392, iPhone10,3) root built locally (`tools/fetch-ios-parts.sh` from Apple's CDN,
  `sudo tools/build-ios-root.sh`: 16 GiB HFSX, 8.9 GiB used). **Download mode mis-writes Android sparse
  images**: Heimdall `--USERDATA root.simg` reported success but USERDATA came out shifted (phone block 0 =
  image block 1280, 5 MiB), so HFS saw "unknown Volume Signature : 0". Heimdall also needs 60 s USB timeouts
  for big partitions (patched locally). Root now goes through TWRP adb: `tools/starlte-flash-root.py adb
  build/root.img` (64 MiB chunks, SHA-256 read-back). The UFS protector after S-Boot is already open
  (region 0 = 0..0xffffffff, all LUNs, ctrl 0xf1; regions 1-7 zero; RSECURITY 0xefe26492).
- 2026-10-25: **milestone 4 reached — iOS root mounted, launchd exec'd.** USERDATA rewritten via TWRP (all
  256 chunks verified), kernel put on BOOT via TWRP `dd` + `adb reboot` (no Download-mode recovery detour).
  Screen: peek USERDATA [1024] 48580005 (HX); `BSD root: ufsroot0`; `hfs: mounted IOSROOT on device b(1, 0)`;
  `root volume is not authenticated (error 25); continuing unsealed`; `load_init_program ... /sbin/launchd`;
  `exec launchd`; then `shared_region: ... [1(launchd)] check_np(...) vm_shared_region_start_address() failed`
  (dyld asking for a not-yet-mapped shared cache). Boot-args: cpus=1 rd=ufsroot0 ufsroot=USERDATA
  cs_enforcement_disable=1 unentitled_ios_sim_launch=1 ignition_level=1 launchd_unsecure_cache=1 wdt=-1
  -wdt_no_pmu wdt_test=no_user_checkin (root read-only, ufsrw pinned off).
- 2026-10-25: launchd stuck in-kernel after exec: sampler kernel backtrace = fleh_synchronous ->
  sleh_synchronous (DABORT_EL1) -> arm_fast_fault, thread runnable forever. Cause: **Cortex-A55 has PAN**;
  SCTLR_EL1_DEFAULT had SPAN=0, so every exception to EL1 set PSTATE.PAN and this A53-derived config never
  clears it -> copyin/copyout permission-fault loop. Fix (STARLTE): SCTLR SPAN set (0x34d4593d) + `msr PAN,#0`
  (.long 0xd500409f) after the SCTLR write in start.s.
- 2026-10-25: **milestone 5 — iOS userland runs**: libignition (hardware target d22ap, "hello from
  launchd.1", shared cache found on the system volume, "ignition sequence complete"), `Darwin Bootstrapper
  Version 7.0.0`, boot tasks fsck (skipped on OS container) and mount-phase-1 -> `mount_hfs: Read-only file
  system`, `mount: / failed with 1` -> panic `mount[3] exited`. Next: UFS writes (USERDATA only) + rootrw.
- 2026-10-25: **milestone 6 — launchd boots the iOS daemon set** with UFS writes (USERDATA-bounded, one 4 KiB
  block per WRITE(10), `ufsrw=1 rootrw=1`): dozens of `exec` (xpcproxy, followupd, seld, familynotificationd,
  mediaremoted, symptomsd, WirelessRadioManagerd, healthd, bluetoothd, cloudd, datamigrator, misd, homed,
  corecaptured, transparencyd, ...); fairplayd.H2 relaunching; hb 11: 253282 UFS cmds (~1400/s); launchd shared
  region slide 0x1330c000; `HEROLTE LOG: alive 180 s` (flash log on). Log pull: `tools/starlte-log-pull.py`.
- 2026-10-25: flash log `device/logs/boot1.txt` (660 s): 184 distinct daemons, 677 execs (xpcproxy 328,
  bluetoothd 50 and fairplayd.H2 36 crash-looping). **SpringBoard (line 669) and backboardd (767) exec'd**;
  backboardd opened AppleCLCD (SweetFramebuffer) and created 1440x2960 BGRA + 1125x2436 IOSurfaces, then FB
  activity stops; no frame presented (console kept drawing). Next boot adds the herolte display args
  (a5fb_keep_on a5fb_noconsole a5fb_perf sweetfb_native) and sweetsample=SpringBoard,backboardd,launchd.
- 2026-10-25: **milestone 7 — iOS 16 LOCK SCREEN on the Galaxy S9.** With `a5fb_keep_on=1 a5fb_noconsole=1
  a5fb_perf=1 sweetfb_native=1`: console -> Apple logo -> lock screen ("Wednesday, December 31", clock running
  4:05 -> 4:06, charging battery icon, camera button, home indicator). Not interactive: no touch/button drivers
  on STARLTE yet (S7 ones compiled out). Clock/date drawn red: check for an R/B channel swap with a known image.
  RTC not set (date = epoch). Next: touch (S6SY761 / sec_ts on HSI2C), GPIO keys, colour check, faster UFS
  (multi-block commands), RTC, more CPUs.
- 2026-10-25: **buttons work** (a5xelte_keys.c with STARLTE pins: ALIVE 0x14050000, GPA0 DAT 0x24 bits 3/4/6,
  GPA2 DAT 0x64 bit 4; Bixby = Home): power 0x30, vol+ 0xe9, vol- 0xea, home 0x40 delivered to iOS.
  Touch (bsd/dev/starlte_touch.c, S6SY761 protocol) first try: every I2C tx stuck (int 0x1, trans 0xa0002,
  byte left in TX FIFO) — S7 timing values put FS3 divider = 1 (~6 MHz). Reworked to the vendor 9810 sequence
  (USI_CON=0, reset, FS3[23:16]=0x21 FS2[7:0]=0xe0 FS1[23:16]=0xf0, CTL=0x8, AUTO; reset before each transfer).
- 2026-10-25: touch still dead after the 9810 bus rework (timing reads back right: FS1 0x1f0ff00 FS2 0x30003e0
  FS3 0x210000, CTL 0x8, CONF 0x980120ff; TRANS 0xa0003 = MASTER_BUSY, state 3, byte stuck in TX FIFO).
  Pin levels (boot5): SCL 0 SDA 0 INT 0 before and after the mux -> **touch rails off after S-Boot** (tsp_io
  S2MPS18 LDO35, tsp_avdd LDO43), as herolte found on the S7. PMIC is on SPEEDY 0x141C0000 owned by ACPM
  (acpm-ipc-channel 2); ACPM IPC: mailbox 0x14100000, APM SRAM 0x02039000+0x30000, initdata at +0x7f00.
  Next: minimal polled ACPM IPC client to enable LDO35/LDO43.
- 2026-10-25: **ACPM IPC from XNU works** (starlte_touch.c acpm_send): framework channels at SRAM+0x5bfc,
  ap_max 13, PMIC ch 2 at 0x5c8c (q_len 5, elem 16, poll); PMICID 0x9; after S-Boot LDO35CTRL 0x2c (1.8 V,
  off), LDO43CTRL 0x30 (**3.0 V**, off — tsp_avdd is 3000000 uV, not 3.3 V). Enable writes: 0x6c=0xec,
  0x74=0xf0 (refuses other programmed voltages).
- 2026-10-25: **milestone 8 — TOUCH WORKS, unlocked to the iOS HOME SCREEN.** ACPM enabled LDO35=0xec,
  LDO43=0xf0; SCL/SDA read 1; S6SY761 boot status 0x20, device id ac 37 61, panel max 4096x4096 (16x33 ch);
  events e.g. raw 2222,4080 -> 781,2948 (orientation right). Very slow: 1 CPU (A55, clock unknown), UFS
  4 KiB/command polled (~3000 cmds/s, ~12 MB/s), CPU rendering, bluetoothd/fairplayd crash loops. Settings
  took ~20 s to react, then black. Next for speed: multi-block UFS commands (128-byte PRDT + VS regs as
  exynos9810-woa), more CPUs (measure MPIDRs; PSCI), CPU clock via ACPM DVFS (ch 5), drop crash-looping
  daemons from launchd.plist (rewrite only changed root chunks with starlte-flash-root.py --diff).
- 2026-10-25: root re-flashed via TWRP with `tools/starlte-tune-ios-root.sh` (drops com.apple.bluetoothd,
  fairplayd.H2, fairplaydeviceidentityd). Multi-block UFS try 1 (S-Boot TXPRDT/RXPRDT=0, 8 entries @128 B):
  multi read "completed" with wrong data, then every command OCS 0xf -> root unavailable panic. Now opt-in
  (`ufsmulti=1`): programs TXPRDT=0x8000000c RXPRDT=0xc (Linux/woa), re-checks single-block, then tests
  128-byte entries; default boots stay single-block.
- 2026-10-25: multi-block try 2 (`ufsmulti=1`, TXPRDT/RXPRDT set 0x8000000c/0xc first): single-block still
  same, but 8 x 128-byte entries -> data differ, then OCS 0xf on everything -> panic. **Multi-entry PRDT is not
  usable on the S-Boot host state**; woa only got it after a full host re-init (HCE, PHY/UniPro, link startup,
  FMP DESCTYPE SMC). Kept opt-in; default boots single-block (`out/xnu-boot-known-good.img`). Alternatives:
  queue many single-entry commands in parallel UTRD slots, or more CPUs.
- 2026-10-25: **CPU numbering measured** (starsmpprobe=1, PC-relative park stub build/stub/park.S): firmware is
  PSCI 0.1 (VERSION/AFFINITY_INFO return -1). CPU_ON(1/2/3) starts A55 cores that read MPIDR 0x81000100/
  0x200/0x300; CPU_ON(0x100/0x101) returns 0 but nothing reported within 300 ms (Mongoose M3: later). STARLTE
  SMP: ADT cpu1-3 reg 0x100/0x200/0x300, SweetCPU PSCI target = phys_id >> 8; first try cpus=4.
- 2026-10-25: **milestone 9 — 4 CPUs** (cpus=4, A55 x4): noticeably faster UI once booted (Settings opens much
  faster, Control Center works); Settings shows iPhone X, iOS 16.7.16; some UI says "iPad" (identity props).
  Kernel image `out/xnu-boot-smp4.img`. Still slow overall: single-block UFS, CPU rendering at 1440x2960,
  unknown CPU clock, no big cores.
- 2026-10-25: scaled canvas (`ADTFLAGS=--scale2`, sweetfb_scale=1 lw=828 lh=1792): much faster UI. Bug found:
  the scaler wrote 2960 rows through the 1792-row default-surface mapping (black lower third, glitches) ->
  separate full-panel mapping. 4 CPUs confirmed in log (cpu1-3 came alive). DVFS: FREQ_GET on ACPM ch 0 gave
  0 kHz -> opt-in only (`starfreq=`); the PMU cycle measurement printed nonsense (to redo).
- 2026-10-25: **Project Starbite branding** (commit 9ba3e8c+): loader splash from BOOT's ramdisk
  (`tools/brand/splash.mjs` -> `to-rgb565.py`, `SPLASH=build/brand/splash.bin tools/make-xnu-boot.sh`),
  console banner. Images: `out/xnu-boot-branded.img`; revert = `git revert` + flash
  `out/xnu-boot-pre-branding.img` (tag `pre-branding`).
- Local kernel builds: `tools/build-xnu-local.sh` (Xcode 15.4 + KDK 13.5 22G74, darwin-xnu-build pinned like
  the fork's CI): full build 128 s.

## Hardware / boot facts (measured on the phone)

- Bootloader G960FXXUHFVG4 (Android 10), sales code NEE. Flashing: Heimdall v2.2.2 (Grimler fork, built from
  source) over Download mode; one session per flash — a `--no-reboot` session leaves the protocol wedged.
- S-Boot accepts a v0 boot header + Samsung DTBH (layout copied from official TWRP starlte, see
  `tools/starlte-pack-boot.py`). Kernel slot = raw arm64 Image, text_offset 0x80000, flags 0xa.
- After a Download-mode flash S-Boot may boot RECOVERY once (`mach_board_main: recovery set!(0x80000)`);
  a normal reboot then takes BOOT.
- Handoff: EL1, MMU off, SCTLR 0x30C5183E (alignment checks on), FDT 0x8A000000, initrd 0x89000000
  (16 MiB window like herolte -> append the payload to the Image), boot CPU Cortex-A55 MIDR 0x410FD051,
  MPIDR 0x81000000 (cores in Aff1), **CNTFRQ_EL0 = 0** (firmware leaves it unset; 26 MHz assumed and the
  countdown matched wall time), 16K pages supported on the A55. DECON trigger read 0x3070 before our 0x1281.
- Framebuffer 0xcc000000 1440x2960 ARGB8888 (also `s3cfb.bootloaderfb` in the stock cmdline).
- PMU 0x14060000: INFORM2 +0x808, INFORM3 +0x80c (0x12345671 = Download mode), SWRESET +0x400.
- S-Boot's own log is readable from Android/recovery in `/proc/sec_log` (lines `[0:  t.ttt] ...`).
- KnoxGuard: units in KG state Prenormal/Active block custom binaries after the first custom boot; they
  must be released by the owner/reseller. A unit in state "Checking" accepted and ran the probe.

## Reference landscape (checked 2026-10-25)

| Repo | State | Use for us |
|---|---|---|
| `oosexgang/hackios-xnu-kernel` | Exists, active (1.3k stars). XNU 8796.121.2 + iOS 16.5. Boots POCO X5 Pro (SD778G) via `fastboot boot` → Mu-Silicium UEFI → `xnuloader`. Host: **Linux x86-64**; issue #6 has open macOS/ARM host fixes | Generic XNU patches, APFS, driver replacements, AMFI/Sandbox stand-ins, userland builder, QEMU bench |
| `oosexgang/hackios-xnu-userland` | Companion repo | iOS image build, injected libs |
| `wardevsa/ios-a5xelte` (`herolte` branch) | Exists, active. **Galaxy S7 (Exynos 8890) to iOS home screen.** S-Boot loads their loader from BOOT. Own Exynos UFS driver, `sec_ts` touch over HSI2C, DECON framebuffer. Lineage: ios-sweet (Redmi Note 10 Pro) → a5xelte (Exynos 7580) → herolte | **Closest model for us**: same vendor bootloader problem, same touch family, Exynos UFS |
| `ntdevlabs/exynos9810-woa` | Exists. **S9+ (star2lte, SM-G965F)**, not S9. EDK2 UEFI flashed to BOOT; Windows drivers for Exynos 9810 UFS, HSI2C, S6SY761 touch | Exynos 9810 UFS + HSI2C + touch register-level reference (same SoC) |
| `sonic011gamer/edk2-exynos` | The brief's `ntdevlabs/edk2-exynos` is 404; this is the real one | Boot shim for S-Boot |
| `ivoszbg/uniLoader` | Exists, has `configs/starlte_defconfig` | Known-good S-Boot → payload layout for **our exact board**: text 0x87000000, payload 0x90000000, framebuffer **0xcc000000, 1440×2960, 4 bytes/px** |
| `vutung2311/starlte` | Exists, last push 2019 (Android P vendor kernel) | Hardware node cross-reference |

No prior iOS/XNU work for Exynos 9810 / starlte found on GitHub (searches: "starlte xnu", "exynos9810 ios").
Press (Oct 2026) on HackiOS: iphonesoft.fr, decrypt.co.

## Corrections to the brief

1. **Boot path.** HackiOS needs `fastboot boot`; Samsung has no fastboot. Our path is S-Boot → our loader in the
   BOOT partition (as herolte and uniLoader do), not HackiOS's UEFI flow. A Mu-Silicium/EDK2 UEFI on BOOT
   (WoA style) is a possible alternative later, but it adds a layer before first light.
2. **Backup partition names.** `persist`, `modemst1/2` are Qualcomm. Exynos Samsung uses `EFS`, `SEC_EFS`,
   (`CPEFS`), `PARAM`, `RADIO`, … — read the real list from `/dev/block/by-name`.
3. **WoA repo is for S9+ (star2lte).** Same SoC, but panel resolution, touch wiring and board GPIOs may differ.
4. **Host.** HackiOS documents Linux x86-64; herolte builds XNU on GitHub macOS runners. An Apple Silicon Mac may
   be able to build XNU natively — to be checked before setting up a VM.
5. **iOS version.** HackiOS = 16.5 (generic), herolte = 16.7.16 iPhone X userland. Choose once the base is chosen.

## Milestones

0. Backup (`tools/backup-starlte.sh`), confirm starlte, dump `/sys/firmware/fdt` and stock BOOT.
1. Our code runs from BOOT: a solid colour on the 0xcc000000 framebuffer (uniLoader-style), then back to TWRP.
2. XNU verbose text on the panel; persistent log readable from TWRP.
3. UFS (read-only first), root fs, launchd.
4. Touch (likely S6SY761 / `sec_ts` over HSI2C — confirm from the DT), then SpringBoard.
