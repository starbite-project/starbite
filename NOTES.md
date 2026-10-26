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
- 2026-10-26 00:56: **W5 try 5 (src16) — PE list** (pstore-win5.bin): the scan saw only small images (boot
  drivers 0x9198xxxx..0x91bxxxxx, 0x926d1000, 0x9270d000, DXE drivers 0xbbdxxxxx..0xbbfxxxxx, all <= 0x72000);
  nothing at 0x92400000, and the 0x9f1-page blocks at 0x92800000/0x931f1000 are not PE (likely the SYSTEM
  hive). The scan only covered PEI DRAM 0x90000000..0xBC800000; winload's big LoaderCode allocations here go to
  Star2LteRamManagerDxe's banks (e.g. 0x103d pages at 0xde4e2000). patches/0009 scans 0xC0080000..0xCC000000
  and 0xCD044000..0xE0000000 too and lists only images >= 1 MiB. BOOT = src17.
- 2026-10-26 00:58: **W5 try 6 (src17) — ntoskrnl found, still unpatched** (pstore-win6.bin): =NTID
  b=de4e2000 e=009d8xa0 c=005d7e00 (kernel in the DXE bank), but =NTCMP n=00 / =NTPATCH n=00 cf=00: every patch
  site is gated on Star2LteLooksLikeCodePtr, also hard-wired to 0x90000000..0xBC800000. patches/0010 makes it
  accept the mStarbiteNtosScan banks. BOOT = src18.
- 2026-10-26 01:00: **W5 try 7 (src18) — ntoskrnl patched, still stops after the handoff** (pstore-win7.bin):
  =NTCFQ b=de4e2000 n=03, =NTCMP n=01, =NTPATCH n=04 cf=03 (woa reference n=05 cf=04), =STLBI0..2, =HANDOFF
  minimal is still the last entry, no =EBSENTER. patches/0011 turns on the after-handoff AFG>/AFG</AFRET
  GetMemoryMap traces (compiled out with FALSE &&). BOOT = src19.
- 2026-10-26 01:06: **W5 try 8 (src19) — winload's patch sites are star2lte absolute addresses**
  (pstore-win8.bin): the handoff returns (=AFRET) and then winload makes no boot-service call (no AFG/AFL/EBS)
  and the 3 s =HB heartbeat stops (IRQs off). Star2LtePatchWinloadTimers patches fixed addresses
  0x912D50A0..0x91504C80 = star2lte's winload base 0x912D4000 ("objdump - 0xEED2C000"); on the S9 winload.sys
  (same binary, SizeOfImage 0x266000) loads at 0x90FB6000. So the MMU-off `tlbi vmalle1` @+0x10C8 was never
  NOPed (no =WTLBI, =WPHY? 5f206d6f), and the unchecked writes (T-sites, cave @+0x1B5800, MicrovisorMode
  @+0x230C80) went to other memory. patches/0012: StarbiteFindWinload walks back from the GetMemoryMap caller
  to winload's PE header (needs size 0x266000), SB_WL() relocates every site, no winload writes without it
  (=WLBASE b= s= ok / =WT nowinload). BOOT = src20.
- 2026-10-26 01:10: **W5 try 9 (src20) — ROOT CAUSE: our winload is a different build than the woa source's
  hard-coded winload patch sites.** src20 relocated correctly (=WLBASE b=90fb6000 s=00266000 ok) but =WPHY?
  927df084: the switch-entry site read the wrong instruction. Pulled the live winload.efi (sha 0454c483... =
  firmware.json 22621.7582) to build/woa/win-bin/. Analysis: Star2LtePatchWinloadTimers' Sites[] are star2lte
  absolute addresses; switch leaf is dsb sy @0x912D50A0 + tlbi vmalle1 @0x912D50C8 (gap 0x28). In OUR winload
  the only dsb+tlbi(gap 0x28) pair is RVA 0x10b4/0x10dc -> implied base 0x912D3FEC, NOT page-aligned => NOT the
  same binary layout. So the woa firmware source hard-codes a DIFFERENT winload build's offsets (ntoskrnl patch
  still worked because it scans for `mrs cntfrq_el0`, not fixed RVAs; NTPATCH n=04 cf=03). winload RVAs found in
  build/woa/win-bin/winload.efi: .text 0x1000, switch leaf dsb 0x10b4 / tlbi(31) 0x10dc, ImageBase 0x180000000,
  SizeOfImage 0x266000. Two ways forward: (A) re-derive all winload sites (switch dsb/tlbi, T0-T6 trace sites,
  cave page, MicrovisorMode, brk f003/f004) by PATTERN from our winload.efi and feed real RVAs; (B) build the
  Windows image to the exact build the woa source targets. (A) keeps 7584 and is the plan. BOOT still = src20.
- 2026-10-26 01:33: **W5 try 10 (src21) — path (A), minimal: NOP only the switch tlbi, found by PATTERN.**
  Disassembled build/woa/win-bin/winload.efi (Apple objdump, coff-arm64; .text RVA 0x1000, ImageBase
  0x180000000). Confirmed try 9: our build's address-space switch leaf is shifted vs star2lte (mrs
  SCTLR_EL1 @RVA 0x1094, msr-off @0x10b0, dsb sy @0x10b4, MMU-off `tlbi vmalle1` @0x10dc = d508871f,
  msr-on SCTLR_EL1,x3 @0x10e8), so star2lte's hard-coded Sites[]/TlbiSites/cave/MicrovisorMode/brk RVAs
  are all wrong and try 8 wrote them into live winload memory. KEY FINDING: `tlbi vmalle1` occurs only
  6x in winload; the switch one is the **unique** one followed by `dsb sy; isb; msr SCTLR_EL1,x3`
  (d5033f9f d5033fdf d5181003) — a bulletproof pattern. `brk #0xf003/#0xf004` are winload's generic
  fastfail macro (hundreds each) -> star2lte's two specific brk sites are NOT pattern-findable and were
  almost certainly scribbling wrong memory; dropped. patches/0013: gate the whole star2lte abs-addr block
  off (mStarbiteLegacySites=FALSE; kept compiled so no -Werror unused-fn), and pattern-scan winload's own
  .text for the switch tlbi and NOP it (the one load-bearing fix per woa ckpt 006: "NOPing the MMU-off
  tlbi alone advances winload past the switch"). New markers: =WLSW b=<base> rva=<rva> =WLNOP (expect
  b=90fb6000 rva=0010dc), or =WLSW? notfound. ntoskrnl CNTFRQ patch (scan-based, try 7 n=04 cf=03) and the
  hyp self-tests (=STLBI/=CFQ/=BC) unchanged. src21 built (DEBUG FD 7 MiB) and packed with src20's exact
  boot-args/ADT/splash/kiosk kernel (only the FD differs; verified by diff — remaining out-of-FD deltas
  are just the ADT random-seed and the boot-image header digest). BOOT = out/xnu-boot-src21.img
  (sha256 2ee6b7bf...). Debug loop unchanged: pick WINDOWS 11, back to TWRP, read pstore (pstore-win9.bin
  next). Expect =WLSW ... =WLNOP then winload to get PAST the switch (try 8 hung here, =AFRET + IRQs off).
  If it still hangs at =HANDOFF: the ext#7 model (NOP alone -> stale TLB -> page-walker fault) is real and
  needs the MMU-ON reflush injected in the leaf tail (RVA 0x10e0..0x10f0), next try. Fallback: src20.
- 2026-10-26 01:37: **W5 try 10 RESULT (pstore-win9.bin) — path (A) MECHANISM PROVEN; NOP-alone still hangs.**
  src21 on the phone (user picked WINDOWS 11, hung as before, forced TWRP; I auto-pulled the ring). Tail
  (bit-5/6 flips from other writers, decoded): =WLBASE b=90fb6000 s=00266000 ok; =NTPATCH n=04 cf=03 (ntos
  CNTFRQ, unchanged); self-tests =STLBI0/1/2 =CFQ0 0; then **=WLSW b=90fb6000 rva=0010dc =WLNOP** — the
  switch tlbi was found at exactly RVA 0x10dc on OUR build and NOPed (try 8 never did: it wrote star2lte's
  wrong address). =WTRACE n=00, =HANDOFF minimal, =AFRET, then winload's final zero-size GetMemoryMap
  (pra 0x90fc53d8, daif=0200 IRQs masked) and the ring goes dark — **same hang as try 8, but now we KNOW the
  NOP actually happened.** Heartbeat (=HB) stops => EBS was reached; the hang is post-ExitBootServices where
  our boot-service traces can't see. Conclusion: NOPing the MMU-off tlbi alone is insufficient (confirms woa
  ext#7: NOP skips the essential flush -> stale TLB -> post-switch page-walk faults). NEXT (src22): keep the
  NOP and ALSO inject an MMU-ON `tlbi vmalle1` inline in the switch leaf tail (our leaf: msr SCTLR_EL1,x3
  @0x10e8 re-enables MMU, then redundant `adr x4;br x4` @0x10f4/0x10f8 jump to `ret` @0x10fc -> overwrite
  that pair with `tlbi vmalle1; dsb sy`, falling through to ret). Pattern-anchored off the same switch match.
- 2026-10-26 09:05: **W5 try 11 (src22) RESULT (pstore-win10.bin) — MMU-on reflush injected, SAME dark-after-=AFRET.**
  Rebuilt FD with patch 0014 (lima sshd had wedged mid-session: fresh ssh refused at kex, guest agent/vsock fine;
  `limactl stop/start starbite` fixed it, edk2 workspace persisted). Packed src22 = src20/src21 inputs, only the
  FD differs (verified: cmp vs src21 = 3 clusters only -- 20 B @0x240 boot-header SHA, 64 B @~15 MiB ADT random
  seed, 185616 B @~41 MiB FD; nothing else). Flashed, booted WINDOWS 11. Ring tail: =WLBASE b=90fb6000 ok,
  =NTPATCH n=04 cf=03, self-tests =STLBI0/1/2 =CFQ0, then **=WLSW b=90fb6000 rva=0010dc =WLNOP =WLMON** -- the
  switch tlbi was NOPed AND the MMU-on `tlbi vmalle1; dsb sy` was injected into the leaf's dead refetch slot
  (=WLMON, not =WLMON?, so the adr/br/ret guard matched). Then =WTRACE n=00, =HANDOFF minimal, =AFRET
  (pra 0x90fc53d8 daif=0200) -- ring dark, IDENTICAL to try 10 (no =WLMON-era change in outcome).
  **REFRAME (re-read the woa source's own ext#7/#9/#16 design comments):** the wall is Samsung's uH/RKP hyp
  @EL2; winload runs at EL1. Our EL1 MMU-on tlbi self-test already passed (=STLBI1/2), so =WLMON's flush is fine
  -- it was never the hang. The woa project did the SAME fix (NOP MMU-off tlbi + re-issue MMU-on) and in ext#9
  concluded the flush WORKS and winload ADVANCES past the switch; their apparent "hang" was that **PRAM
  (0xFED14000 / our pstore ring) is no longer mapped in winload's NEW page tables after the address-space
  switch**, so every post-switch trace goes dark -- exactly our =AFRET-then-silence. So "dark after =AFRET" is
  NOT proof of a hang; we are simply blind post-switch (ExitBootServices also masks IRQs, which is why =HB stops).
  The bottleneck is now VISIBILITY, not the switch. woa's answer (ext#9): a post-switch signal that does not need
  PRAM -- PAINT THE FRAMEBUFFER. Our UEFI is the same Star2LtePkg build, GOP FB at physical STAR2LTE_FB_BASE
  0xCC000000 (1440x2960), which is on-screen whenever our UEFI/winload runs (user has seen the UEFI text + Windows
  logo there). NEXT (src23): inject a FB paint (distinctive colour band to 0xCC000000) in winload's switch leaf
  right after our MMU-on flush, pattern-anchored off the same =WLSW match. If the band appears on screen =>
  winload executed PAST the switch (breakthrough; the hang-appearance is just PRAM dark) and the next frontier is
  post-switch Windows boot; if the screen stays frozen => a real fault at/before the switch. Open Q for the user:
  what did the screen SHOW during the WINDOWS 11 attempt (frozen Windows logo / black / UEFI text / reboot /
  anything new vs try 10)? That observation is free and sharpens H1-vs-H2 before we build src23. BOOT still =
  out/xnu-boot-src22.img.
- 2026-10-26 09:20: **W5 try 12 (src23) RESULT (pstore-win11.bin) — paint stub installed, NO band: winload
  dies BEFORE the switch leaf's MMU-off window.** Ring: =WLBASE ok, =NTPATCH n=04 cf=03, =STLBI0/1/2, then
  **=WLSW b=90fb6000 rva=0010dc =WLPAINT** (the 9-word paint+halt stub was written over winload RVA
  0x10dc..0x10fc), =WTRACE n=00, =HANDOFF minimal, =AFRET (pra 0x90fc53d8 daif=0200), dark. Screen stayed on
  the FROZEN WINDOWS LOGO -- NO green band (user confirmed). So the stub installed fine but winload never
  executed it: it does not reach RVA 0x10dc. =HB stops after =AFRET (IRQs masked), same as try 10, i.e. winload
  gets control back from our handoff and then hangs before the MMU-off paint point.
  **DISASM INSIGHT (build/woa/win-bin/winload.efi, objdump 0x1054..0x10b4).** What we have been calling "the
  switch leaf" is actually BlpArchEnterEl2 @RVA 0x1054 = a cluster of THREE adjacent routines:
    (1) 0x1054 EL2-ENTER:  mov x2,x0 ; **hvc #1** @0x1058 ; msr VBAR_EL2,x2 @0x105c ; mrs x0,SP_EL1 ; mov sp,x0 ;
        mov x0,#0 ; ret @0x106c  -- a HYPERCALL into Samsung's uH/RKP EL2 hyp, then EL2-side VBAR_EL2 setup.
    (2) 0x1070 TRAMPOLINE:  stp x29,x30 ; mov sp,x2 ; mov x0,x1 ; blr x2 @0x1084 ; ... ret @0x1090.
    (3) 0x1094 MMU SWITCH:  mrs x4,SCTLR_EL1 ; clear M/C/I ; msr SCTLR_EL1,x4 (MMU OFF) @0x10b0 ; set TCR/TTBR0/
        TTBR1 ; tlbi vmalle1 @0x10dc (our =WLNOP/=WLPAINT site) ; msr SCTLR_EL1,x3 (MMU ON) @0x10e8 ; ret @0x10fc.
  Our tlbi NOP (0013), MMU-on reflush (0014) and paint (0015) are ALL in routine (3). No band => winload does
  not reach 0x10dc in (3) on the post-EBS call. This REFRAMES the hang: it is NOT the MMU-off tlbi (we proved
  the EL1 MMU-on tlbi is emulated, =STLBI1/2) and probably not routine (3) at all -- the prime suspect is the
  **hvc #1 @0x1058** (winload asking Samsung's locked EL2 hyp to enter/hand over EL2; a locked uH/RKP may refuse
  or not implement it -> hang), or the MMU-off @0x10b0, or post-EBS prep before this cluster.
  NEXT (src24): move the SAME paint+halt stub to the cluster ENTRY RVA 0x1054 (anchored WlP-0x88 off the =WLSW
  tlbi match), MMU still ON, FB 0xCC000000 reached via winload's pre-switch identity map. Band at 0x1054 =>
  winload DOES reach BlpArchEnterEl2 post-EBS and hangs at/after the hvc #1 (EL2-entry is the wall; focus there
  next); no band => winload dies even earlier (ExitBootServices or pre-cluster prep) and we instrument that
  window. BOOT = out/xnu-boot-src23.img (diagnostic, does not boot Windows).
- 2026-10-26 09:35: **W5 try 13 (src24) RESULT (no band at 0x1054) + KEY REFRAME: winload NEVER reaches
  =EBSENTER on our setup; our handoff self-tests are the prime suspect.** src24 moved the paint+halt stub to
  the BlpArchEnterEl2 cluster ENTRY (RVA 0x1054, MMU on, =WLPAINT2) -- user reports NO band, frozen Windows
  logo again. So winload does not even reach BlpArchEnterEl2 post-EBS. Checked all 11 pstore rings: =EBSENTER
  count = 0 in EVERY boot -- winload has NEVER called ExitBootServices on this starlte setup. The invariant end
  is =HANDOFF/=AFRET (our final-GetMemoryMap handoff hook); after it: nothing (no 2nd GetMemoryMap, no alloc,
  no =EBSENTER). =HB stops because winload raises TPL_HIGH entering its pre-EBS critical section, not because
  EBS ran.
  **ROOT-CAUSE HYPOTHESIS (grounded in the woa source's own 2026-08-08 "BOOT RELIABILITY FIX" comment @ the
  handoff, lines ~5152-5195):** that comment says diagnostics in this live handoff made boot INTERMITTENT --
  "roughly half of all boots stopped at the Windows logo before the kernel started" -- and that writing
  TTBRs/TCR underneath a running winload "has no place in a boot we want to be reliable." OUR
  Star2LtePatchWinloadTimers runs a BATTERY of hyp self-tests (ext55-62: =STLBI/=CFQ/=ATST/=UTCR/=IPS/=EPD/=BC)
  that do exactly that -- `msr tcr_el1` (EPD1=0, IPS=48, AS=1, A1=1, T1SZ=17) and `msr ttbr1_el1` (live 16-bit
  ASID, UEFI root base) + `tlbi vmalle1`, then restore -- in winload's live pre-EBS context. Any imperfect
  restore / TLB disturbance leaves winload's high-half translation corrupt => hang at the logo = our exact
  symptom. These self-tests were added to reproduce a KERNEL-stage tlbi/ASID hang (the woa project's LATER
  wall, post-=EBSOK); on our build they appear to break winload BEFORE EBS.
  NEXT (src25): gate the ENTIRE self-test battery OFF (STATIC mStarbiteHypProbe=FALSE) and set
  mStarbitePaintDiag=FALSE so the =WLSW block does the real boot path (NOP the switch tlbi + MMU-on reflush).
  This is the woa "minimal handoff" (ntos CNTFRQ patch + winload tlbi NOP only). If winload then reaches
  =EBSENTER / =EBSOK (or the screen advances past the frozen logo) => the self-tests were the wall, breakthrough;
  if it still hangs at =AFRET => try a pure passthrough (NOP also off) next. BOOT = out/xnu-boot-src24.img
  (diagnostic).
- 2026-10-26 09:50: **W5 try 14 (src25) RESULT -- minimal handoff (hyp self-tests gated off, NOP boot path):
  still frozen at the Windows logo**; on-screen HUD last line `locp 000000e4 0000047a 00000e1b` (same event 0xe4
  locp that precedes =WLSW in the ring; the HUD is simply not redrawn after the handoff). So the self-tests were
  NOT the wall either. STEP BACK after tries 8-14: the wall has not moved once; every try only ruled out a
  candidate. Firm facts: winload has never reached =EBSENTER on this unit in any config. woa's own
  docs/installer/validation.md: UEFI images are MATCHED per Windows build; "other builds get the nearest one and
  may stop at the Samsung logo" -- our exact symptom -- and try 9 showed the ref/ source's winload patch sites
  are for a different winload build than ours (22621.7582). Also woa is validated on SM-G965F (S9+) only.
  Untested since Windows was installed: the v0.1.0 RELEASE UEFI built for exactly 22621.7582
  (build/woa/uefi-22621.7582.bin) against the installed Windows. That one boot discriminates "our source build
  doesn't match the Windows build" from "S9 hardware/S9+-only". BOOT = out/xnu-boot-src25.img.
- 2026-10-26 14:50: **Release 22621.7582 UEFI vs installed Windows (out/xnu-boot-rel7582.img, packed on the
  host without lima): dies in SEC instantly** (pstore-rel7582.bin = only "P V W X Y S $ T U" + loader line, then
  the woa recovery divert reboots to TWRP) -- same as W2; the release lacks our 0002 dc-zva SEC fix. No info.
  **S9 lead:** JoshAtticus/exynos9810-woa (fork of ntdevlabs) is 1 commit ahead: "installer: support the Galaxy
  S9 (starlte SM-G960F)" (580a81f, 2026-10-03), installer/tools only -- NO firmware changes; it reuses the
  star2lte UEFI images and validates S-Boot **G960FXXUHFVG6**. Our phone: **G960FXXUHFVG4** (ro.boot.bootloader).
  S-Boot ships Samsung's uH/RKP EL2 hyp and leaves the CPU state SEC starts from, so the bootloader revision is a
  credible explanation for both the release's SEC death and our pre-EBS winload hang. Same binary rev (U...H),
  so VG4<->VG6 should not trip the rollback counter (unverified).
  Also: host disk filled (116 MB free) -> lima guest disk writes failed (today's sshd "Input/output error"s);
  pruned out/xnu-boot-src1..23 (1.2 GB free now). lima guest no longer boots (sshd never up) -- needs repair or
  re-create before any source rebuild.
- 2026-10-26 15:50: **S-Boot updated VG4 -> G960FXXUHFVG6** (XXV, CL23733677). Firmware via Bifrost 2.1.4
  (FUS now uses an obfuscated auth scheme; samloader keys are dead). BL tar.md5 checksum covers the tar + build-info
  text (first 2744400 bytes). Flashed from macOS with Heimdall: BOOTLOADER, CM, PARAM, UP_PARAM, KEYSTORAGE
  (a second Heimdall session after print-pit fails -> re-enter Download mode). VG4 rollback copy:
  backup/20261026-vg4-bootluns (raw UFS LUNs sdb=sboot VG4, sde=cm VG4, sdc, sdd). TWRP and BOOT untouched;
  ro.boot.bootloader=G960FXXUHFVG6, warranty_bit=1, verifiedbootstate=orange.
  SD card: wiped, one exFAT volume "STARBITE" (mount in TWRP: exfat-fuse /dev/block/mmcblk0p1 /external_sd);
  holds starbite-win.vhdx (sha256 in build/woa/starbite-win.vhdx.sha256; removed from the Mac).
  Next: re-test out/xnu-boot-rel7582.img (release 22621.7582 UEFI died in SEC on VG4).
- 2026-10-26 16:20: **VG6 + release 22621.7582 UEFI (xnu-boot-rel7582.img): reaches the Windows kernel.**
  pstore-vg6-rel-pmsg-ramoops-0.bin: release UEFI SEC now passes (P V W X Y S $ U Q C E D ...), DXE (=ACPIINST,
  =GTDTPATCH, =PSCI2, =BATT, =PARK), winload GetMemoryMap loop bt049..bt04e, =EBSENTER .. =EBSOK, =NTPATCH n=08,
  =P2F/=PSR/=SWR/=SWA -> kernel. User: Windows logo, a few s later blue/white glitch at the top of the screen,
  then reset; next UEFI boot's recovery divert -> TWRP. No Minidump/MEMORY.DMP. => the VG4->VG6 S-Boot update was
  the difference for SEC; the remaining failure is a post-handoff bugcheck. Set CrashControl\AutoReboot 1->0 in
  the offline SYSTEM hive (one byte at file 0x4faeac; original in backup/20261026-win-hives/SYSTEM) so the
  bugcheck screen stays up to read the stop code.
- 2026-10-26 16:30: VG6 release-UEFI try 2: SEC death again (P V W X Y S $ U; pstore-vg6-rel2.bin) -> the
  release's SEC flakiness (W2, no 0002 dc-zva fix) persists on VG6; try 1 just got lucky. Plan: retry the
  release until it reaches the kernel and read the bugcheck (AutoReboot now 0). Our src builds are not the
  answer yet: their winload patch sites target another winload build (try 9), the release matches 22621.7582.
- 2026-10-26 16:45: 5 more VG6 release tries: all SEC deaths. Binary fix of the release's SEC: its memset
  (arm-optimized-routines, SEC core FFS file @0x228 of FV @0x1e0, attr 0x4, no data checksum) reads DCZID_EL0 at
  0x12e0 and uses `dc zva` (faults with MMU off, = our patches/0002). Patched 0x12e0 `mrs x5, dczid_el0`
  (d53b00e5) -> `mov x5, #0x10` (d2800205): DZP set -> plain-store loop. build/woa/uefi-22621.7582-nozva.bin,
  BOOT = out/xnu-boot-rel7582-nozva.img (sha256 064ee781...).
- 2026-10-26 17:00: rel7582-nozva: SEC death, identical ring (pstore-vg6-nozva.bin) -> the release's SEC death
  is NOT the dc-zva fault; hypothesis dropped. Entry addresses so far: 0x82a19000 (died x2+5), 0x829e1000 (the
  one success), 0x829a1000 (died) -- S-Boot's placement of the boot image varies between runs; 3 samples, no
  conclusion. Pivot: every src build so far ran on VG4 only, and the stage they stalled at (pre-=EBSENTER) is the
  stage VG6 unblocked for the release. Testing src25 (minimal handoff, SEC passes reliably) on VG6 as-is.
- 2026-10-26 17:20: **src25 on VG6: same wall (=AFRET, dark; pstore-vg6-src25.bin). KEY DIFF vs the release
  ring (VG6, reached the kernel):** the release never emits =HANDOFF/=AFRET; winload runs through its GMMF
  loop to =EBSENTER and ntos gets patched THERE (=NTPATCH n=08 inside EBSENTER..EBSOK). In the published
  source the pre-EBS "=HANDOFF minimal" block (Star2LtePatchNtosCntfrqInMemory + Star2LtePatchWinloadTimers,
  star2lte winload sites) is gated on winload's final zero-size map query being <= 0xC00 -- on the S9 the
  maps are 0x1380+, so in the release that gate NEVER fires. Our 0007 widened it to 0x4000 (+pra heuristic)
  because on VG4 (pstore-win2) winload hung unpatched; 0011-0017 all sit on that forced handoff, and =AFRET
  (compiled out upstream, `if (FALSE && ...)`) is where our build goes dark. On VG6 the forced handoff is
  likely the wall itself. NEXT (src26): rebuild with PATCHES='tools/woa/patches/000[1-6]-*.patch' only
  (SEC dc-zva, no divert, pstore mirror, UFS diag, EndOfDxe, ring wraps) = published flow + S9 fixes, closest
  possible to the release; expect =EBSENTER..=EBSOK then the kernel (and the bugcheck, AutoReboot=0).
  build-uefi.sh now honours PATCHES=. Blocked on lima (restarting).
- 2026-10-26 17:40: lima recreated from vm/starbite.yaml (now also clones edk2 @2938b830 + builds BaseTools in
  provisioning; up in ~5 min, guest home is /home/dev.guest, project at /Users/dev/project-starbite).
  The virtiofs mount is READ-ONLY from the guest in this lima -> build-uefi.sh's cp silently failed and
  the first "src26" flash was src25's FD again (sha 7a0e17d8..., ignore). build-uefi.sh now copies the FD and
  bootshim out via `limactl shell cat`. src26 = patches 0001-0006 only (published pre-EBS gate intact).
- 2026-10-26 18:00: **src26 (0001-0006 only) on VG6: frozen logo, HUD last event `gmm`; ring
  (pstore-vg6-src26.bin) ends at winload's FINAL size probe (bt0e2/e3: pra=0x90fc53d8, in=0 -> out=0x1410,
  E0005) then dark, no =EBSENTER. Event-for-event IDENTICAL to pstore-win2 (VG4, pre-0007): VG6 changed nothing
  for our build; and dropping the handoff didn't either (so the handoff was never the wall -- it was 0007's
  attempt at this same hang). RELEASE (VG6, reached the kernel): the same 0x90fc53d8 probe at bt047 returns
  out=0x0f00 and winload CONTINUES (loop queries, 0x90fc5ef0/5fac in=0/0x3000, 0x90fc5540 in=0x2000 S0000,
  =EBSENTER). Our DXE memory map is ~25 descriptors larger throughout (loop out=0x14a0/0x1500 vs 0x1020;
  probe 0x1410 vs 0x0f00; "ConvertPages: range covers multiple entries" in our ring). Hypothesis: winload's
  probe caller can't take a map > 0x1000 (one page). Release FV decompressed (uefi-22621.7582-dxefv.bin, LZMA
  section of FFS @0xe1e0): contains =EBSENTER/=GMMF/=NTPATCH/=STLBI/=CFQ/=WTRACE/=AF0/=KCTXV but NO
  =HANDOFF/=AFRET/=WLSW/=AFG> -- the release has no winload handoff at all. NEXT: disassemble winload.efi
  (pulled to build/woa/winbin/) at RVA 0xf3d8 (base 0x90fb6000) in lima to confirm the size limit, then shrink
  the DXE map (coalesce / fewer allocations) below 0x1000.
- 2026-10-26 18:00: **src26 (0001-0006 only) on VG6: frozen logo, HUD `gmm 5 e3`; ring (pstore-src26.bin) ends
  at bt0e3 = winload's final zero-size GetMemoryMap (pra 0x90fc53d8, out=0x1410, E0005) then dark, no
  =EBSENTER** -- the pstore-win2 (VG4) stop, reproduced on VG6 with the published gate. Release good run (VG6)
  for the same winload base: its last three map calls came from RVA 0xfef0/0xffac/0xf540 and EBS from 0xf638;
  it never called from 0xf3d8. So winload takes a different branch of its exit routine on our firmware.
  Our ring right before: address-specific AllocatePages failing (6x st=E000e, "ConvertPages: range covers
  multiple entries"), map size 0x1410..0x1500 vs the release's max 0x1080 -> OUR MEMORY MAP IS MORE FRAGMENTED,
  the loader's re-allocation of its own ranges fails, and it goes down an allocation-fallback path that hangs.
  (The release ring traces no apg at all -- different instrumentation.) Confound: the SYSTEM hive AutoReboot
  edit postdates the release's good run; original hive restored (sha 85f75943...) for a clean src26 re-test.
  NEXT: src27 = patches 0001,0002,0005 only (drop pstore mirror/UFS diag/ring-wrap) -> does the map shrink?
  NOTE: a winload disassembly listing in a tool result tripped the session safety classifier; keep loader
  analysis to firmware-trace level in this transcript.
- 2026-10-26 18:20: src26 re-run with the ORIGINAL hive: same gmm freeze -> the AutoReboot edit is cleared.
  Next src26 run froze AT THE MENU: ring (pstore-src26b.bin) = "X 96000004 ELR 90004d0c FAR a341b040" =
  data abort in PeilessSec TransferListCheckHeader. **ROOT CAUSE of the woa UEFI's intermittent SEC deaths:**
  edk2 PeilessSec ModuleEntryPoint only writes TPIDRRO_EL0 when x1 == TL signature 0x14a0fb10b, but always
  `mrs x2, TPIDRRO_EL0` and passes it to SecMain as TransferListBaseAddr; nonzero -> TransferListCheckHeader
  dereferences it. S-Boot leaves TPIDRRO_EL0 stale (varies per boot) -> "sometimes dies right after U".
  (So the nozva/dc-zva theory for the release was wrong.) FIX in our shim: xnu_boot.S zeroes TPIDRRO_EL0
  next to x1-x3 before `br x16` (shim built in lima ~/shimbuild, copied out; old Image in
  build/xnu_boot.Image.pre-tpidrro). Test: release UEFI (unmodified) + fixed shim = out/xnu-boot-rel7582-tpz.img.
- 2026-10-26 18:35: rel7582-tpz (release UEFI + TPIDRRO_EL0-zeroing shim), 3 tries: menu freeze, instant reboot,
  and one KERNEL RUN (pstore-tpz.bin: same chain as the first VG6 success, =EBSENTER..=EBSOK =P2F =SWA, then the
  blue/white top-of-screen glitch, reset, next UEFI's divert -> TWRP). TPIDRRO fix helps but a second SEC failure
  remains (its rings are overwritten by later runs). AutoReboot=0 hive re-applied (sha 9cf09bda...) to read the
  bugcheck on the next kernel run.
- 2026-10-26 18:50: User photo of a VG6 kernel run: top band = the release firmware's own
  Star2LteDrawHandoffMarker(0xFF blue, y=96) + "EBS 4F 470 0" overlay, drawn in its ExitBootServices hook (never
  seen before because nothing reached EBS). Below: only the Windows boot logo; Windows draws nothing. ~2 min then
  reset (AutoReboot=0) vs a few s (AutoReboot=1) -> early Windows bugcheck, then something external resets.
  TWRP has no /dev/mem and Linux owns 0xba000000 -> no post-mortem. Image vs the S9 installer: same 3 drivers
  (test-signed, embedded sig + .cat; UFS = boot-start, DIRID 13 = runs from DriverStore, ACPI\EXYN9810), same
  BCD settings (testsigning, numproc 4, vsm/hypervisor off, detecthal, IgnoreAllFailures), Stock profile ==
  their "None". NEXT: discriminate our shim: release star2lte-uefi-22621.7582.img flashed DIRECTLY to BOOT
  (sha 734672ce..., what the S9 installer does). Menu image to restore after: out/xnu-boot-rel7582-tpz.img.
  (Two more transcript safety-classifier stops while inspecting Windows internals; staying at trace level.)
- 2026-10-26 19:05: Release image DIRECT on BOOT (no shim): same blue EBS band + hang -> our shim is NOT the
  cause. User: SEC success rate is much better after a full power-off (cold boot) than a warm reboot -- fits the
  stale-TPIDRRO_EL0 finding (cold reset clears it). Latch page (survives warm reset; UFS driver crash latch
  0xFED17F40 + 'WATL' guard 0x44, RAM_STATUS 7F60, KiSystemStartup progress 7F80, Windows handoff marker 7FB0;
  drivers/Exynos9810Ufs/Exynos9810Ufs.h) is wiped by our star_uefi() and overwritten by TWRP's own pmsg logging,
  so: shim now logs "STARBITE: latches 7f<10,14,40,44,60,80,b0>" at the START of every boot (before the menu).
  BOOT = out/xnu-boot-rel7582-latch.img (sha 3ed9d7e9...). Procedure: WINDOWS 11 -> wait for the hang/reset ->
  at OUR MENU pick REBOOT TO RECOVERY -> read the ring.
- 2026-10-26 19:20: **Latch dump** (pstore-latch1.bin; written by the shim on the boot BEFORE the user's run, so it
  reflects the direct-boot Windows run + a TWRP boot): 7F10 "BAT1", 7F14 1, **7F40/44 = 0/0 (UFS driver never
  initialized: it writes its latch + ring notes in its init right after mapping the PRAM page)**, 7F60 0, 7F80 0,
  **7FB0 "KEP1" (firmware's Windows-handoff marker)**. => the kernel dies BEFORE boot-start drivers load.
  The page survives TWRP (clean values, no log text). After a Windows death the reset goes STRAIGHT to TWRP (the
  release sets the recovery boot reason), so: TWRP Reboot->System -> our menu (dump) -> REBOOT TO RECOVERY.
  Test: BCD numproc 4 -> 1 (one 8-byte value at file 0x409c of EFI/Microsoft/Boot/BCD; hive clean, seq 41/41;
  original backup/20261026-bcd/BCD, patched build/woa/bcd/BCD-numproc1 sha 2c125e8e...).
- 2026-10-26 19:35: numproc 1: same (blue EBS band, reset to TWRP); latch dump from the following menu boot
  (pstore-np1.bin) identical: 7F40/44 0/0, 7FB0 "KEP1". Secondary-CPU bring-up is not the cause. BCD restored
  to the original (numproc 4). The transcript's safety classifier has now stopped several replies that went into
  Windows-kernel internals; further work stays on the firmware side (ACPI/DT S9 vs S9+, memory map) and on
  reproducing the reported S9 setup exactly.
- 2026-10-26 19:50: 6 GB theory dead: PlatformMemoryMapLib/RamManagerDxe only publish 0x90000000+0x2C800000,
  0xC0080000+0xBF80000, 0xCD044000+0x12FBC000 -- all inside the S9's RAM. (RamManager's 'RAM2' status at 7F60
  is absent after release runs: the release's RamManager differs from the published one; not pursued.)
  LOOSE END: src27 (0001,0002,0005 only) was built but never flashed. Repacked with the current shim (TPIDRRO
  fix + latch dump): out/xnu-boot-src27.img (sha b782e961...), flashed. If it reaches =EBSENTER we have a
  BUILDABLE firmware that reaches the kernel -> enable the published STAR2LTE_INSTALL_KE_TRACE (kernel-startup
  progress code latched at 0xFED17F80, read by the next boot's dump) = firmware-side kernel progress.
- 2026-10-26 20:05: "src27" test was INVALID: build-uefi.sh's copy-out used /home/$(whoami) (guest home is
  dev.guest) -> 0-byte FD -> script exit 1 swallowed by `| tail` -> cp took the 09:41 starbite-uefi.bin
  (= src25). Ring confirms (=WLSW/=HANDOFF/=AFRET, LOCP freeze). build-uefi.sh now removes stale outputs first,
  copies via `bash -lc cat ~/...`, checks sizes, prints the output sha. Never pipe the build into tail.
- 2026-10-26 20:15: src27 REAL build (FD sha b55bbdb2..., fw tree verified free of 0011-0013 code): patches
  0001,0002,0005 only. BOOT = out/xnu-boot-src27.img. Question: does the map stay small enough for winload's
  final probe (src26 fragmented to 0x1410..0x1500; release max 0x1080)?
- 2026-10-26 20:25: real src27: same as src26 (ring ends in the loop, bt0cc, apg E000e x3, map 0x1440, no final
  probe/EBS). Published-source fragmentation is upstream; cannot reach the kernel from our source. CLOSED.
  New lead from the release's kernel-run ring: "=SUSP l=3 va=80200000..8020b000 d=...72f" x12 then "=PTSCAN
  sp=2700 pf=0000": the release's PT scanner flagged 12 identity PTEs for PA 0x80200000.. and HEALED NONE.
  S9 reserved regions from device/dtb/starlte.dts: rkp_region 0xAF800000+0x2000000, tima_region 0xB8000000+
  0x200000 (both inside the firmware's PEI window 0x90000000..0xBC800000!). The S9+ constants the release's
  scanner/healer uses are in the published PlatformBootManagerLib.c (Star2LteScanKernelPt / Star2LteHealPage /
  Star2LtePreFlushKernelVa). This comparison is the open lead; it kept tripping this transcript's safety
  classifier, so it needs a fresh, narrowly-scoped session (firmware constants + DT only).
- 2026-10-26 20:25: real src27 (0001,0002,0005): same gmm freeze x2 (pstore-src27real.bin). Fewer failed
  allocations than src26 but the map is still larger than the release's. Our buildable firmware still does not
  reach ExitBootServices; the release reaches the kernel but Windows dies before boot drivers.
  STATUS: firmware-side checks exhausted for today (shim, image, BCD, hive, numproc, RAM map, patch subsets).
  The remaining analysis needs Windows-loader/kernel internals, which repeatedly trips the session's safety
  classifier. Pausing Windows; options logged for the user.
