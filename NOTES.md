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

## Windows 11 multi-boot plan (2026-10-25, research only, nothing flashed)

Goal: boot menu with iOS 16, Linux kiosk, Windows 11 ARM64 (and Android 16 later) on this S9 (starlte).
Reference: `ref/exynos9810-woa` (v0.1.0), which is for the **S9+ (star2lte, SM-G965F) only**.

Facts that shape the plan:
- woa UEFI = bootshim + 7 MiB EDK2 FD run at 0x90000000, flashed **instead of** our loader on BOOT. To keep the
  menu, our loader must chainload the FD (copy to 0x90000000, jump) as it does the kiosk Linux. **BOOT is 55 MiB
  and `out/xnu-boot-kiosk.img` is already 50 MB** -> the FD does not fit until the kiosk kernel shrinks
  (26.7 MB raw Image: compress it or trim the config).
- Each UEFI build patches the Windows boot loader and kernel in memory for **one exact build** (22621/22631.2428
  or .7582/.7584). The current Microsoft ARM64 ISO (24H2+) will not boot; 23H2 media at one of those builds is
  needed (UUP dump).
- woa writes NTFS over **all of USERDATA** (GPT GUID addressed from the BCD), ESP on CACHE, second copy on SYSTEM.
  Here USERDATA holds the iOS root (0..16 GiB) and the kiosk root (+17 GiB, 1.15 GiB), CACHE is only 12 MiB
  (S9+ docs say 600 MiB) and SYSTEM is needed for Android 16 later. -> **USERDATA has to be split in the GPT**:
  keep USERDATA's start and name (iOS `ufsroot=USERDATA` and kiosk +17 GiB keep working), shrink it to ~19 GiB,
  add WINESP (FAT32, 4 KiB sectors, ~256 MiB), WINDOWS (NTFS) and a reserve for Android 16 data.
  USERDATA now: sda25, 4 KiB blocks 1698432 + 13917563 (53.09 GiB). Samsung's PIT keeps the old layout: never
  flash USERDATA by name from Download mode after this (we already use TWRP).
- S9 vs S9+: same SoC, UFS host, HSI2C, S6SY761 touch, 1440x2960 at 0xcc000000, GIC, PSCI. Different: RAM
  (3.7 GB seen here vs 6 GB; the default 711 MiB / deployed 1357 MiB stages sit below 0xBC800000, the 4 GiB-bank
  stage must stay off), disk geometry (`PartitionMap.cs` DiskBytes/WindowsOffset are star2lte values), board
  GPIOs/keys, ACPI DSDT. Port = a StarLtePkg copy of Star2LtePkg plus installer geometry.
- The Windows image step (DISM, VHDX, bcdboot) needs Windows: a Windows 11 ARM VM on this Mac or the laptop.
  The UEFI builds with EDK2 + clang (could be done on the Mac).

Order: W1 star2lte UEFI chainloaded from the menu (BOOT only, known-good image as fallback) -> W2 StarLtePkg
(RAM map, geometry) to the UEFI boot menu with UFS -> W3 GPT split (backup GPT + EFS first) -> W4 build a 22631
image for the WINDOWS geometry, copy via TWRP -> W5 first boot, OOBE. Android 16 later: SYSTEM/VENDOR/ODM + the
reserve partition.
- 2026-10-25: **W1 reached — S9+ UEFI runs on the S9 from the boot menu.** Menu row WINDOWS 11 (Download mode
  moved into Starbite Engineer) enters the v0.1.0 star2lte-uefi-22621.7582 bootshim+FD from an "SBUEFI!!" bundle
  (`tools/woa/get-uefi.sh`, `UEFI=build/woa/uefi-22621.7582.bin tools/make-xnu-boot.sh`). Phone showed the
  star2lte UEFI text, "no bootable device", then an automatic reboot (expected: no Windows yet). Room on BOOT:
  overlay RLE (`tools/brand/rle.py`, 6.6 MB -> 0.72 MB, `SPLASH=build/brand/overlay.rle`), kiosk kernel without
  camera/MFC/vision/NFC (`tools/kiosk/build-kernel.sh`; knox/modem/SSP are link dependencies). Image
  `out/xnu-boot-windows.img` 51.4 MB of 57.7 MB; fallback `out/xnu-boot-kiosk.img`.
- 2026-10-25: **W2 analysis — the star2lte UEFI needs no hardware changes for starlte.** Built it from source in
  lima (`tools/woa/build-uefi.sh`): edk2 master 2938b830 (2026-06-19, after DefaultExceptionHandlerLib was merged,
  before GptLib), clang 18 CLANGDWARF; FD strings match the v0.1.0 release except its DEBUG_CLANGPDB path. Checked
  against device/dtb/starlte.dts: DRAM (PEI 0x90000000..0xBC800000, DXE adds 0xC0080000..0xCC000000 and
  0xCD044000..0xE0000000) avoids every S9 fixed carve-out (rkp/tima handled in the map, seclog 0xC0000000, modem
  0xF0000000+, abox, secure_camera 0xE1900000, exynos_ss 0xFDA00000); touch = S6SY761 @0x48 on HSI2C 0x104B0000
  (IRQ 436) with GPA1.0 (IRQ 76) exactly as in the DSDT; pstore at 0xFED10000 on both. BDS has no partition
  names/GUIDs hard-coded (stock PartitionDxe + FAT), so a new WINESP works. Open: whether UFS high-LBA reads work
  on this unit (read the UEFI's pmsg log in TWRP after a WINDOWS 11 boot).
- 2026-10-25: **W2 blocked — the v0.1.0 star2lte UEFI is unreliable on this S9.** Only the first run (W1) reached
  BDS. Later runs, read from TWRP's /sys/fs/pstore/pmsg-ramoops-0 (ramoops pmsg 0xFED14000, header "DBGC"
  sig/start/size; loader now appends "STARBITE: entering UEFI ..." there before the handoff): either dies in SEC
  right after the unpublished breadcrumbs "P V W X Y S $ T U" (before ArmPlatformInitialize's 'C'), or gets
  through PEI and ACPI install (=ACPIINST/=GTDTPATCH) and dies in DXE dispatch after 16 driver starts (next in FDF
  order: Star2LteRamManagerDxe, UFS). Both modes also happen with the release image flashed straight to BOOT
  (no Starbite loader), so it is not the chainload. Ruled out: woa boot-attempt counter (BAT1 @0xFED17F10, loader
  now clears 0xFED17F0C..0xFED18000 before every handoff), FAT test volume on CACHE, bank-1 RAM (S9 /proc/iomem:
  80000000-bc7fffff, c0000000-dfffffff, e1900000-ff7fffff, 880000000-8ffffffff). Resets carry INF2 0x12345678 /
  INF3 0x12345674 (same values as the woa recovery divert and our menu's RECOVERY). The release has
  instrumentation not in the published source (provenance "WORKTREE" hashes); our source build
  (tools/woa/build-uefi.sh, CLANGPDB, edk2 2938b830) hangs on the phone before drawing. Traces kept in
  build/woa/pstore-*.bin. Test ESP tool: tools/woa/ufs-test.sh (CACHE backed up/restored).
- 2026-10-25: **W2 reached — our own source build of the woa UEFI boots on starlte to BDS** ("no bootable
  device. Connect UFS/USB", stays on the panel: patches/0001 turns the TWRP diverts off). Debugged through the
  pmsg ring (loader resets it and writes "STARBITE: entering UEFI ..." before the handoff; edk2 breadcrumbs,
  catch-all EL1 vectors logging "X ESR ELR FAR SPSR", and patches/0003 mirroring all DEBUG output into the ring,
  newest 16 KiB kept). Fixes, all from source (tools/woa/build-uefi.sh, CLANGPDB, edk2 2938b830 + patches-edk2):
  1. SEC/PEI must not use BaseMemoryLibOptDxe: its ZeroMem `dc zva` takes an alignment fault with the MMU off
     (ESR 0x96000061 at PeilessSec+0x13c8 zeroing a page table) -> patches/0002 (SEC BaseMemoryLib).
  2. CNTFRQ_EL0 = 0 from S-Boot: TimerDxe programmed a zero period and the first tick stormed forever right after
     the Timer arch protocol install. Re-created the woa team's unpublished edk2 edit (ArmGenericTimerGetTimerFreq
     falls back to 26 MHz) -> patches-edk2/gen-cntfrq.py.
  The v0.1.0 release's flakiness on this unit is not explained yet (its winload patching is build-specific, so
  our build must eventually match a Windows build too). Side note: stretches of the ring show bit-5/6 flips
  (something else writes the exynos_ss log page at times).

## HANDOFF (2026-10-25 23:30) — where W2 stands, how to continue

State: BOOT = out/xnu-boot-src11.img (menu: iOS / Linux kiosk / WINDOWS 11 / TWRP / Engineer). WINDOWS 11 boots
OUR source UEFI (tools/woa/build-uefi.sh DEBUG -> build/woa/Star2LtePkg.fd, paired with the v0.1.0 release's
480-byte bootshim: build/woa/starbite-uefi.bin; then `UEFI=build/woa/starbite-uefi.bin` + the usual
CMDLINE/ADTFLAGS=--scale2/SPLASH=build/brand/overlay.rle/LINUX=build/linux-bundle/Image for tools/make-xnu-boot.sh;
flash with tools/twrp-flash-boot.sh from TWRP). CACHE holds the disk-test ESP (EFI Shell + startup.nsh,
tools/woa/ufs-test.sh install|results|restore; original CACHE in build/woa/cache-backup.img — restore when done).
Windows 22631.7584 ARM64 ISO is built inside the UTM VM "Starbite Windows" (Downloads\22631.7584_..._convert).

Debug loop: user picks WINDOWS 11, firmware runs, user goes back to TWRP (Power+VolDown, RECOVERY); read
`adb exec-out cat /sys/fs/pstore/pmsg-ramoops-0` (raw copies: build/woa/pstore-*.bin; newest = pstore-src11.bin). Loader writes "STARBITE: entering UEFI" first; edk2 DEBUG (errors+warnings) and the BDS "=..."
markers follow. The ring sometimes shows bit-5/6 flips (other writers); harmless.

Fixes so far (all reproducible): firmware patches tools/woa/patches/0001 (no TWRP diverts) 0002 (SEC plain
BaseMemoryLib, no dc zva) 0003 (DEBUG->pstore ring, errors+warnings, ASSERT print) 0004 (aligned UFS diag read);
edk2 patch tools/woa/patches-edk2/0001-*.patch generated by regen.sh from gen-crumbs/gen-vectors/gen-cntfrq
(26 MHz CNTFRQ fallback)/gen-ufs (Exynos byte-granular UTRD, resp 0x400, PRDT 0x800, 128-byte PRDT entries of
4 KiB, RuL 0x400, <=512 KiB per command = the UFS driver's WC bounce buffer). edk2 in lima ~/work/edk2 @2938b830.

Result chain: UFS init OK, whole disk seen (LastBlock 0xEE47FF), both GPTs valid, partitions enumerated, FAT on
CACHE mounted (=FATSCAN p15 ...r0F, =WINFS fs=01), boot option \efi\boot\bootaa64.efi registered and tried; the
shell load failed with ACCESS_DENIED (=BMBOOTRET st=E000f) after "A19 FALL why=04 len=104000" (1 MiB read bigger
than the 512 KiB bounce). src11 caps commands at 512 KiB: "A19 FALL" is gone (all reads use the 512 KiB WC bounce @0x97f80000, A19 BNC=97f80000 sz=080000), FAT mounts and =BMBOOT n=01 tries the shell, but it still returns =BMBOOTRET st=E000f (EFI_ACCESS_DENIED) plus "ASSERT_EFI_ERROR (Not Found)" in BdsDxe BmDriverHealth.c(551). So the read size was not the cause. NEXT: find who returns ACCESS_DENIED for the image: check the PlatformBootManagerLib boot path around RegisterWindowsBootOption / EfiBootManagerBoot (~line 7181) and any LoadImage/StartImage hooks or image-hash allowlist (the woa UEFI only accepts its exact winload/kernel builds), SecurityStubDxe/DxeSecurityManagementLib, and whether the read data is correct (compare a file's bytes). Quick test idea: patch the platform to boot the shell even if not a Windows image, or log LoadImage status.
After the shell test works: W3 (split USERDATA: keep USERDATA start/name ~19 GiB, WINESP FAT32 4K ~256 MiB,
WINDOWS NTFS ~26 GiB, ~8 GiB reserve for Android 16), W4 (build image in the VM from the 22631.7584 ISO with the
woa installer's flow, our geometry), W5 first boot. Nothing has been committed since 43878ae — commit soon.
- 2026-10-25 23:35: **ACCESS_DENIED found:** DxeCore LoadImage -> SecurityStubDxe Defer3rdPartyImageLoad denies
  every non-FV image until EndOfDxe is signalled, and the woa PlatformBootManagerLib never signals it (no
  security handler, no hash allowlist involved). patches/0005 signals gEfiEndOfDxeEventGroupGuid at the end of
  PlatformBootManagerBeforeConsole (as ArmVirtPkg does), marker "=ENDOFDXE". BOOT = out/xnu-boot-src12.img
  (same CMDLINE/ADTFLAGS/SPLASH/LINUX as src11). Expect the EFI Shell disk test to run; log -> pstore-src12.bin.
- 2026-10-25 23:40: **W2 done — the EFI Shell runs from UFS.** src12 (patches/0005): =ENDOFDXE, LoadImage of
  \EFI\BOOT\BOOTAA64.EFI st=S0000 (1 MiB file), startup.nsh ran to "done" and wrote results.txt on CACHE
  (build/woa/ufstest-results.txt; pmsg build/woa/pstore-src12.bin). Map shows all 25 LU0 partitions + LU1..LU4/
  0xC4. Whole-disk BlockIo reads at LBA 0, 2, 0x19EA80, 0x5DEA80, 0xEE47FF (backup GPT) are byte-identical to
  TWRP `dd` of /dev/block/sda. (Partition reads beyond their size fail as expected; the script's `echo ----`
  labels fail because the shell's echo takes them as flags.) CACHE restored afterwards. The BmDriverHealth(551)
  "Not Found" ASSERT is non-fatal. Windows: the UTM VM "Starbite Windows" has finished building the 22631.7584
  image too. Next: W3 GPT split (back up GPT + EFS first), then W4 copy the image to WINDOWS/WINESP.
- 2026-10-25 23:45: **W3 prepared, nothing written.** Backup backup/20261025-233855 (backup-starlte.sh: EFS,
  CPEFS, PARAM, UP_PARAM, BOOT, RECOVERY, DTB, DTBO, RADIO, KEYSTORAGE, PERSISTENT, STEADY, OMR + raw primary/
  backup GPT of sda..sde). Measured: iOS HFS+ "HX" = exactly 16 GiB (9.43 used), kiosk ext4 at +17 GiB =
  1.148 GiB -> ends at 18.15 GiB. sda GPT: header size 512, 128 entries at LBA 2, usable 6..0xEE47FA.
  tools/woa/gpt-split.sh gen builds the new GPT offline (lima losetup 4K + sgdisk) -> build/woa/gpt-new:
  25 USERDATA 1698432..6679295 (19 GiB, same start/name/GUID 3502F564-...), 26 WINESP 256 MiB EF00
  (D84E28FA-6593-407F-B843-D2DB04378402), 27 WINDOWS 25.8 GiB (1FB16851-063B-4D98-9CE9-49EF71D08603),
  28 A16DATA 8 GiB (CBC93D1E-...). Entries 1-24, disk GUID, protective MBR byte-identical; CRCs valid.
  `apply` writes both copies from TWRP and reads them back. W4's BCD must use the WINDOWS/WINESP GUIDs
  above, not woa's PartitionMap.cs ones (those are star2lte's).
- 2026-10-25 23:50: **W3 done — GPT split applied.** `gpt-split.sh apply` wrote both copies, read-back identical;
  after a reboot to TWRP the kernel shows sda25 USERDATA / sda26 WINESP / sda27 WINDOWS / sda28 A16DATA at the
  planned starts and sizes (by-name links present). iOS HFS+ header unchanged; kiosk ext4 `e2fsck -fn` clean.
  Restore path: backup/20261025-233855/gpt-sda-{primary,backup}.bin (dd to LBA 0 and 15615995).
  Next W4: format WINESP (FAT32, 4K) and WINDOWS (NTFS), apply the 22631.7584 image from the VM, bcdboot with the
  GUIDs above.
- 2026-10-26 00:10: **W4 — Windows image built and being written.** WINESP grown to 512 MiB (256 MiB is under
  FAT32's 65525 clusters at 4 KiB): USERDATA now 1698432..6613759 (18.75 GiB), WINESP 6613760..6744831
  {b3babc53-99be-42c7-b0c1-c1df6350e566}, WINDOWS 6744832..13518591 {923ecef4-5f78-46b6-b695-2aede847e27c},
  A16DATA {bafcb27c-...}; disk GUID {52444e41-494f-2044-4d4d-43204449534b} (same as star2lte).
  VM build: tools/woa/vm-build-image.sh -> tools/woa/vm/build-image.ps1 (UTM guest agent as SYSTEM; utmctl exec
  does not wait, so scripts log to C:\sb and the host polls). No Hyper-V on ARM -> 4Kn dynamic VHDX via
  virtdisk CreateVirtualDisk v2; ESP/NTFS at the phone's exact byte offsets; Stock profile: Pro index 1, woa
  drivers.zip (Hsi2c/Ufs/S6SY761 /ForceUnsigned), unattend (user starbite, no password, autologon, 275 DPI,
  FLE Standard Time), bcdboot, woa BCD settings (testsigning, numproc 4, ...) + GPT-qualified devices on our
  GUIDs. winload.efi/ntoskrnl.exe 10.0.22621.7582, sha256 0454c483.../75481a55... = firmware.json 22621.7582
  exactly. Windows volume 13.86 GiB used. VHDX (15.1 GB) to the Mac over vmnet: VM curl -T to a PUT receiver on
  192.168.64.1 (230 MiB/s; the guest agent does 0.35 MB/s). qemu 8.2 refuses the 4Kn VHDX (ENOTSUP), so
  tools/woa/vhdx.py reads it; tools/woa/write-windows.sh streams payload blocks over adb exec-in, zero-fills
  the rest on the phone, sha256 per 256 MiB piece. (TWRP sh arithmetic is 32-bit: do byte math on the Mac.)
- 2026-10-26 00:30: **W4 done — Windows 22631.7584 written to the phone.** write-windows.sh: WINESP 64 MiB
  payload + 448 MiB zeros, WINDOWS 14268 MiB payload + 12192 MiB zeros, every 256 MiB piece sha256-checked,
  boot sectors and NTFS backup boot sector re-checked. Bugs fixed on the way: adb exec-in drops the tail of long
  streams (now adb push to /tmp + local dd); adb calls inside `while read` ate the run list (fd 3 now, plus a
  coverage check). TWRP mounts both read-only: EFI\Boot\bootaa64.efi, EFI\Microsoft\Boot\{bootmgfw.efi,BCD},
  \Windows, Panther\unattend.xml. BOOT unchanged (src12). Next W5: pick WINDOWS 11.
- 2026-10-26 00:40: **W5 try 1 — Windows logo, no spinner, hang** (forced reset after several minutes). HUD:
  "gmm 5 000000E3 0000046A 00000DFE" = last trace tag gmm, trace event 0xE3, block-read seq 0x46A, alloc seq
  0xDFE. bootmgfw loaded/started, winload ran (variables, CI policy, 1.8 MB load, GOP), last ring entry bt0ad
  "gmm>" (3rd late GetMemoryMap, caller 0x910b50a0) cut mid-line: woa's BdsPramByte has a LINEAR 0x3F00 cap and
  the ring was full (pstore-win1.bin = 16128 bytes), so ~54 later events (=HANDOFF minimal, EBS) were dropped.
  The Windows UFS driver resets the ring when it starts; the ring held only firmware trace -> the kernel never
  reached boot drivers (woa's own note at the =HANDOFF code: "full firmware trace but zero UFSINIT", ~half of
  boots, retry boots). patches/0006 makes BdsPramByte wrap like 0003 (newest kept). BOOT = src13.
- 2026-10-26 00:46: **W5 try 2 — same hang, cause found.** With 0006 the ring kept the tail (pstore-win2.bin):
  last event bt0e3 = winload's final zero-size GetMemoryMap (pra 0x90fc53d8 < ra 0x90ffc768) returning
  out=0x1410. The handoff (=AF0 / Star2LtePatchNtosCntfrqInMemory + Star2LtePatchWinloadTimers, "=HANDOFF
  minimal") is gated on *MemoryMapSize <= 0xC00 (star2lte's size), so on the S9 it never ran and winload hung
  unpatched. patches/0007 raises the gate to 0x4000. BOOT = src14.
- 2026-10-26 00:50: **W5 try 3 (src14) — handoff fired too early, hang sooner** (pstore-win3.bin): =AF0 at
  winload's FIRST late zero-size query (out=0x1380, pra 0x90ffa8a0), =STLBI0..2 =CFQ0 =HANDOFF minimal, then
  winload kept allocating and died at bt0bd. Neither try reached =EBSENTER, so the patches must go in at the
  final query, not ExitBootServices. Same pattern every boot: first pra 0x90ffa8a0 (< ra 0x90ffc768), loop pra
  0x910b50a0 (> ra), final pra 0x90fc53d8 (< ra). patches/0007 (renamed final-map-query) now requires a loop
  query before the final one (size gate kept loose at 0x4000). BOOT = src15.
- 2026-10-26 00:53: **W5 try 4 (src15) — handoff at the right query, ntoskrnl not found** (pstore-win4.bin):
  =AF0 at the final query (pra 0x90fc53d8, out=0x1470), =STLBI0..2, =CFQ0 0, but `=NTPATCH n=00 cf=00 i=1c`
  (woa reference: n=05 cf=04 i=26 b=92400000): the DRAM scan 0x90000000..0xBC800000 saw 0x1c PE images and
  none validated as ntoskrnl, so the kernel got no CNTFRQ patch. Last entry =HANDOFF minimal, no =EBSENTER.
  winload allocated 0x92400000 (0x400 pages) and 0x92800000 / 0x931f1000 (0x9f1 pages each) before the
  query. patches/0008 lists every scanned PE (=PE b s m x name). BOOT = src16.
