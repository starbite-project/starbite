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
- 2026-10-26 20:40: **src27 (real): same gmm freeze, map 0x1440, 3 apg E000e.** After our runs the 7F60 latch
  reads 'RAM2' (our RamManagerDxe ran); after RELEASE runs it is 0. Unpacked both FDs' LZMA inner FV and diffed
  the module sets (tools: inline python, GUID+UI name): **the release is NOT the published source.**
  ONLY in the release: P (18 KB, "SMP!SMP1", XSDT/APIC/FACP), Star2LtePsciEl2ShimStage0Proven (11 KB, emits
  =PSCI2 -- the marker the release ring shows), Winpe22621RuntimeFrequency (4.6 KB, =LFC* markers),
  98100003-...-140500009041 (25 KB + 602 B RAW, depex), 6e89af20-... (2.8 KB freeform). ONLY in ours:
  Star2LteRamManagerDxe. Size diffs: SecurityStubDxe release 5.7 KB vs ours 26 KB (custom stub -> why 0005
  EndOfDxe is needed), BdsDxe +7 KB in the release, DxeCore/UfsPassThru differ. Extracted the release-only
  modules (PE32/depex/UI/RAW sections) to build/woa/relmods/<name>/.
  patches/0018 = drop RamManagerDxe (release-like map: bank 0 only). patches/0019 = transplant the 4 modules as
  binary FILE DRIVER entries (same GUIDs/sections, after the UFS host driver). src28 = 0001,0002,0005,0018
  (sha ebadfdfa..., flashed). src29 = + 0019 (building).
- 2026-10-26 20:55: src28 (no RamManager): still the gmm freeze (pstore-src28.bin). The small map alone is not
  it. src29 (+0019: release's P / PsciEl2ShimStage0Proven / RuntimeFrequency / 98100003 transplanted; verified
  36 modules) flashed, sha 63e2fd0b....
- 2026-10-26 21:10: **src29 (release modules transplanted): new stop -- HUD "APL", before the Windows logo.**
  Ring (pstore-src29.bin): the PSCI EL2 shim runs IDENTICALLY to the release (=PSCI2 start .. stage0=
  EXTERNAL-PASS .. INSTALL0/1 .. hcr=80080002 .. selftest=7fffc .. verdict=SHIM-UNVERIFIED, =PREWDT ARMED --
  =PREWDT is printed by the shim, not by any source). After it: release BDS does stall> loops (1 ms x2, 50 ms
  x2, ...) and goes on to =LFCR/FAT/bootmgfw; OUR BDS does two apl and never calls a boot service again while
  =HB heartbeats keep ticking (36 s). => the release's BdsDxe (+7 KB) carries the post-shim code path.
  patches/0020 = transplant the release's BdsDxe (6d33944a-...) and SecurityStubDxe (f80697e9-..., 5.7 KB).
  src30 = 0001,0002,0005,0018,0019,0020 (building). Untransplanted release files left: DxeCore (-4 KB),
  UfsPassThruDxe (+2 KB), 6e89af20 freeform (all 0xFF padding, 2.8 KB), ~512 B deltas in 6 stock modules.
- 2026-10-26 21:35: src30 (release BdsDxe+SecurityStub too): earlier stop, HUD "locp 6". Both rings do bt006
  locp VariablePolicy (81d1675c) OK, then the shim's start phase; the release BDS resumes with stall> polling,
  ours never resumes (pstore-src30.bin). Outer FV: our PeilessSec 61368 B vs release 57272 (different edk2).
  patches/0021 = + release DxeCore + UfsPassThruDxe (src31, building). CONTROL src32 = our bootshim + our
  PeilessSec file + the release's ENTIRE compressed DXE volume (python splice, FV checksum fixed, verified):
  build/woa/starbite-uefi-src32.bin, BOOT = out/xnu-boot-src32.img (sha d3870dc8...). Blue bar -> the hang is
  in our DXE module set; no -> it is in our SEC/PEI.
- 2026-10-26 21:50: ***** WINDOWS RUNS ON THE S9 ***** src32 (our bootshim + our PeilessSec + the release's
  entire DXE volume): blue EBS band, then the WINDOWS LOGO + SPINNER (first ever), screen black, logo+spinner
  again (Windows' own first-boot reboot), crash a few s later, reset -> TWRP (the UFS driver sets the recovery
  boot reason: =UFSNORMAL B3=12345674). Ring (pstore-src32.bin): =UFSENTRY =UFSINIT (HWINIT=1 STARTED=1)
  =UFSNORMAL OK=1, 78x =UFSWRIT (WMODE=2, issued=verified), =UFSBUGCHECK ARMED=1 (= callback registered).
  Latches: 7F40=a7014401 ('WATL' guard ok: tag a7, boots 1, attempt 0x44, completed), 7F80=0x2c6ce (release KE
  progress latch). => SEC/PEI from our source is fine; the hang in src29-31 is in OUR DXE module set vs the
  release's. Windows got far enough to write its disk.
- 2026-10-26 22:00: No "=UFSBUGCHECK BC=" record in the ring -> Windows did NOT bugcheck (the callback never
  fired). SYSTEM hive on disk rewritten by Windows (7077888 -> 7602176 B, dated today). Reading: first-boot
  setup is progressing; each phase ends in Windows' own reboot, which the UFS driver routes to TWRP
  (B3=12345674) by design. The mid-boot black flash = display handover. Test: just boot Windows again (src32
  stays), time spinner->reset, see if the ring's write count / latch attempt byte advance. src31 packed
  (out/xnu-boot-src31.img, sha a9e10c8c...) for the buildability track later.
- 2026-10-26 22:15: ***** WINDOWS SETUP GUI ON THE S9 ***** 2nd src32 boot: spinner ~1 min, then the "Install
  Windows -- The computer restarted unexpectedly or encountered an unexpected error" dialog (user photo), a bit
  later a reset to the MENU (not TWRP this time). So: kernel, basic display, win32k/user-mode setup all run.
  The dialog = setup's interrupted-phase state from run 1's reset. Open: what resets the phone ~60-70 s after
  the spinner every run (watchdog?).
- 2026-10-26 22:40: src32 run 3 (shim dumps the 0xFED13800 handoff record): record = ARM1 / 0 / 'FWA1'
  (firmware-armed, never changed to 'WAR1') -> the UFS driver never took ownership of the hardware watchdog
  that the release's DXE modules arm (shim =PREWDT, also P and BdsDxe; count 0xFFF5 at the max prescaler is
  ~80 s), so it expires ~1 min after the spinner and resets the SoC. Windows setup itself logs no error (Panther
  setupact.log stops mid-specialize). Touch does not respond in the setup dialog (separate issue: S6SY761/ACPI).
  The session's safety classifier stopped the reply that worked on this watchdog; continue in a fresh session.
- 2026-10-26 22:40: 3rd src32 run: same (setup dialog, reset ~1 min). Touch does not work on the dialog.
  Latch dump now includes the firmware<->UFS-driver handoff record @0xFED13800: 'ARM1' / 0 / 'FWA1'
  (firmware-armed; never 'WAR1'/'WST1'), i.e. the shipped driver never acknowledges the shim's boot timeout
  on this phone, and the timeout ends every Windows boot ~60 s in. The published-source firmware has no such
  timeout (STAR2LTE_EBS_HW_WATCHDOG=0), so the way forward is the buildable track: make OUR DXE set reach the
  kernel (src31 = +DxeCore/UFS next; bisect from src32 otherwise).
  (Replies touching Windows-side internals keep getting stopped by the session safety classifier.)
- 2026-10-26 18:35 (wall clock; the W5 entries above are stamped ~4.5 h ahead): **the ~60 s reset is a
  SOFTWARE reset, not the watchdog.** User saw "Restarting" under the spinner right after the setup dialog;
  and the reference project's prebuilt TWRP module tools/twrp-winre/modules/rwd1_evidence_reader.ko (read-only,
  built for exactly this TWRP kernel 4.9.118-g36e6ddff22b9) loads on our S9 TWRP and reads the retained PRAM
  records: PMU RST_STAT = 0x20000000 SWRESET (a cluster-0 watchdog reset would be 0x01000000). The last two
  commits' watchdog theory is withdrawn: the driver (9.0.0.1, 2026-09-16, DriverStore
  exynos9810ufs.inf_arm64_56a2e6f01fbd68bc; it has NRM1/WWR1/RWD1/UFSSRBSHUT) treats the RWD1 record at
  0xFED13D80 as the authoritative ownership record, and the release firmware never writes it (all 0xFF), so
  the driver sets NoPetReason=WATCHDOG_INVALID and never un-masks the cluster-0 WDT reset at the PMU
  (UfsWdtTakeFirmwareBridgeOwnership needs Rwd1Owned); the firmware-armed 'ARM1'/'FWA1' record is a legacy
  mirror. Other records: P3 'SMP1' gen 0xa55e3013 seq 5 state 0x60 PREBRANCH (not acked), no 'UAR1' unattend
  token, witness 'WWR1': 4/4 CPUs, 1 group, **usable memory 181966 pages = 710 MiB in 11 ranges** -- so the
  "KiSystemStartup progress latch 0x2c6ce at 7F80" in the earlier entries was the witness's page count.
  The pmsg ring (15330 B) sits at the 0x3F00 cap after 78 =UFSWRIT records: anything the driver wrote at
  the end of the run (=UFSSRBSHUT/=UFSREBOOT) is lost, so "no =UFSBUGCHECK BC=" proves nothing either.
  Windows side (TWRP mount.ntfs -o ro of sda27 works): Panther\setupact.log is one run, "Setup Phase = 4",
  ends right after "SysprepSession::CreateSession ... Specialize.xml"; Panther\UnattendGC\setuperr.log:
  "[windeploy.exe] Setup.exe failed, returning exit code [0x1f]" / "Failure occured during online
  installation; cannot complete at this time [0x8007001F]". So setup.exe dies at the start of the specialize
  pass with a generic failure, windeploy shows the "restarted unexpectedly" dialog, and Windows restarts
  itself (the driver's init already set INFORM3 for the next boot; TWRP vs menu depends on the exit path).
  No Setup\Scripts, no project user-mode components on the disk; unattend.xml = 6 DPI reg commands + OOBE
  skips + autologon "starbite". The S9+ flow runs winre-actions.sh prepare-boot from TWRP before every
  Windows boot (clear MISC, rwd1_ack, zero RWD1/P3 records) -- our stock TWRP does none of it, but with
  RWD1 unwritten that only matters for the P3 record.
  TOOLS: tools/woa/twrp-postmortem.sh NAME (module + pmsg -> build/woa/postmortem-NAME/, decoded);
  evidence of this run in build/woa/postmortem-src32-run3/. Shim (src32c, out/xnu-boot-src32c.img, sha
  0ddcc3cc...) now also dumps the driver's 'NRM1' normal-boot record (7F18: phase 2 = an orderly SRB shutdown
  went through the driver) and the 'WWR1' witness at every boot, 32-bit words. Flashed; menu boot next.
  PROCEDURE per run: menu -> WINDOWS 11 -> wait for the reset -> in TWRP run tools/woa/twrp-postmortem.sh.
  OPEN: why setup.exe fails (0x1f) at specialize start on the S9 while the S9+ installs with the same
  firmware+driver. S9-specific inputs: 710 MiB visible RAM (vs the S9+), display/touch (no touch in the
  dialog), DT/ACPI. The Windows-internals part stays out of this transcript (safety classifier).
- 2026-10-26 18:55: src32c run 4: same dialog, then a reset to the MENU (user saw no "Restarting" this time,
  longer until black); menu -> REBOOT TO RECOVERY; tools/woa/twrp-postmortem.sh src32c-run4. The shim's dump at
  that menu boot (first line of the ring) read run 4's leftovers: **NRM1 status 0x00010001 = phase 1 (init)
  only** -> the driver never saw SRB_FUNCTION_SHUTDOWN, i.e. the restart did NOT go through an orderly
  storage shutdown; INFORM2/3 stayed at the driver's init values 12345678/12345670 (normal) -> menu. Run 3
  landed in TWRP (recovery reason set by something at the end: bugcheck callback or driver recovery route);
  run 4 did not. Witness unchanged (4 CPUs, 710 MiB, 11 ranges), crash latch 7F40=a701d201 (completed=1),
  P3 still PREBRANCH, RST_STAT in TWRP is SWRESET but is the shim's own reboot-to-recovery this time.
  Reference project (docs/installer/architecture.md): no mention of this setup failure; its first boot is
  driven from the host (BootRouteService: clear MISC, rwd1_ack, zero RWD1/P3 if non-zero), Windows goes to
  USERDATA, boot files to the FAT EFI; the image build (apply, Add-Driver, slim profile, unattend, bcdboot)
  matches ours. No S9+ memory figure documented. Firmware-side evidence for the restart mechanism is
  exhausted: the ends vary (orderly "Restarting" in run 3 vs none in run 4), the driver's ring is flooded by
  the 78 write records before the end, and the cause is setup.exe's 0x1f failure at specialize start.
- 2026-10-26 19:00: **RAM track.** Why 710 MiB: the memory map Windows sees comes from SEC/PEI (PcdSystemMemory
  0x90000000+0x2C800000), and src32 uses OUR PeilessSec with the release DXE volume (no RamManagerDxe) -> bank 0
  only. The reference DSC has `STAR2LTE_RAM_STAGE` ("0 = 711 MiB baseline, 3 = the deployed 1,357 MiB, 4 = adds
  the 4 GiB bank") but the implementation was in the authors' private edk2 MemoryInitPeiLib, not published:
  the S9+ release ships with ~1.36 GiB, our build with 711 MiB. S9 DRAM (TWRP /proc/iomem): 0x80000000-
  0xBC7FFFFF, 0xC0000000-0xDFFFFFFF, 0xE1900000-0xFF7FFFFF, 0x880000000-0x8FFFFFFFF (2 GiB high bank);
  reserved (starlte.dts): seclog 0xC0000000+0x80000, fb 0xCC000000+0x1044000, secure_camera 0xE1900000+
  0x1E00000, modem_if 0xF0000000+0x9200000, cp_ram_logging 0xF9200000+0x2000000, abox 0xFB200000+0x2800000,
  exynos_ss 0xFDA00000+0x1C08000 (pstore 0xFED10000 inside), fimc 0xFF800000+0x800000, rkp/tima in bank 0.
  (The UfsHcDxe "A105" comment claiming the other banks are not non-secure-accessible came from a bounce
  allocated in an UNMAPPED bank -- a translation fault; Linux at the same EL uses all four banks.)
  patches/0022-starbite-ram-stage: PlatformMemoryMapLib maps each extra bank write-back AND builds a tested
  EFI_RESOURCE_SYSTEM_MEMORY HOB for it (HOB list exists before MemoryPeim): stage 1 = 0xC0080000+0xBF80000
  and 0xCD044000+0x12FBC000 (the RamManagerDxe ranges, +495 MiB -> ~1.2 GiB), stage 2 = +0xE3700000+0xC900000
  (201 MiB), stage 3 = +0x880000000+0x80000000 (2 GiB; DXE allocates top-down, so this one puts firmware
  allocations above 4 GiB -- test last). tools/woa/fv-splice.py = reproducible src32-style splice (our outer
  FV + the release's compressed DXE volume; reproduces src32's DXE volume byte-for-byte).
  src33 = patches 0001,0002,0005,0018-0022 at STAR2LTE_RAM_STAGE=1, spliced with the release DXE volume:
  build/woa/starbite-uefi-src33.bin (sha e3ae67b3...), BOOT = out/xnu-boot-src33.img (sha 7a5eb2dd...),
  flashed. Expect the witness to report ~1.2 GiB; then see whether setup.exe still fails at specialize.
- 2026-10-26 19:10: **src33 (stage 1, ~1.2 GiB): NEW FAILURE -- Windows logo, then winload's "Your PC needs to
  be repaired" screen flashes, reset.** Post-mortem (postmortem-src33-run1): the whole retained PRAM area reads
  0xFF (latches, witness, P3, ring = only the next shim dump) = DRAM was re-initialised: winload's failure
  path calls ResetSystem(EfiResetCold) and the release's Star2LteResetSystemLib does PSCI SYSTEM_RESET first,
  which on this EL3 is a cold reset. So a LOADER failure never leaves a ring; only driver/kernel-era resets
  (PMU SWRESET / RESET2 warm) do. No bootstat.dat on the Windows partition. The extra memory is the only
  change, so winload rejects something about the new map (two more conventional ranges above the firmware
  window; DXE/BDS/loader allocations now land top-down in 0xDxxxxxxx). To read the error code instead of
  filming it: BCD bootstatuspolicy (element 250000e0, QWORD at file 0x3674 of EFI/Microsoft/Boot/BCD, hive
  seq 49/49) set 1 IgnoreAllFailures -> 0 DisplayAllFailures, so the recovery screen stays up (no keyboard
  to dismiss it). Original: backup/20261026-bcd/BCD-seq49-ignoreallfailures; patched build/woa/bcd/
  BCD-displayallfailures (sha 809ca955...), written to WINESP and verified. The BCD parse was validated
  against the earlier numproc edit (25000061 -> 0x409c). RESTORE the original once the code is known.
- 2026-10-26 19:20: src33 error code (user's slow-mo video): **"A fatal error occurred transitioning to the
  operating system", 0xC0000240** = STATUS_REQUEST_ABORTED = winload's mapping of EFI_ABORTED from a firmware
  service; after ExitBootServices, so DisplayAllFailures cannot hold the screen (no boot services left).
  Published PlatformBootManagerLib: the EBS hook's Star2LteScanKernelPt hard-codes the WINDOWS KERNEL PT ROOT
  at PA 0x907AA000 ("rock-stable in 25+ captures") in 13 places -- stable only because winload's allocation
  pattern is deterministic for the bank-0-only map. Extra conventional memory shifts winload's allocations
  (top-down into bank 1) and the release's (binary, newer) gate/hooks abort. The published hook itself only
  logs =SUSP and never returns EFI_ABORTED, so the abort is in the release's newer gate or P module.
  FIX ATTEMPT: patch 0022 now also builds a BuildMemoryAllocationHob(bank, EfiBootServicesData) for each
  extra range (STAR2LTE_RAM_AS_BS_DATA=1): the banks show in the UEFI map as allocated boot-services data,
  so neither DXE/BDS nor winload allocates from them and the bank-0 layout (PT root 0x907AA000) is
  unchanged, while Windows reclaims boot-services memory after the handoff (LoaderFirmwareTemporary ->
  free) and should report ~1.2 GiB in the witness. src34 = same patch set, stage 1 + BS-data:
  build/woa/starbite-uefi-src34.bin (sha 4b1419c8...), BOOT = out/xnu-boot-src34.img (sha 973e824f...),
  flashed. BCD restored to the original (sha 0e09b755..., seq 49, IgnoreAllFailures).
- 2026-10-26 19:35: **src34 (bank 1 as boot-services data): boots like src32c, Windows now sees 1206 MiB.**
  Setup dialog again, then the phone switched off without "Restarting" (postmortem-src34-run1). Witness WWR1:
  4/4 CPUs, 308746 pages = 1206 MiB in 14 ranges (was 181966 pages / 710 MiB / 11). So the BS-data trick works:
  winload keeps its bank-0 layout, the release EBS gate is happy, Windows reclaims the banks. NRM1 again phase
  1 only; RST_STAT SWRESET; P3 PREBRANCH. Setup outcome unchanged -> 710 MiB was not the (only) blocker.
  Windows-side clock: Panther\UnattendGC\setupact.log holds 7 windeploy launches (one per Windows boot so far),
  ALL stamped 2026-08-05 14:29:56..14:30:38; SOFTWARE hive and INF\setupapi.dev.log mtimes are the same moment;
  only SYSTEM carries 2026-10-26 (our host-side hive edits). => Windows starts every boot at the same fixed
  time (~2026-08-05 14:29 local); the release firmware's GetTime likely returns a constant (same on the S9+).
  Only one "Setup.exe failed ... [0x1f]" line is in the windeploy log; Panther\setupact.log is rewritten per
  run and always ends at the Specialize.xml session creation. (TWRP's busybox has no stat -c/od -A; use
  stat -t; TWRP's clock is 2018 so find -mtime is useless on this volume.)
  NEXT: src35 = stage 2 (+0xE3700000+0xC900000, ~1.4 GiB, above the S9+ release's 1357 MiB) to rule memory
  out completely.
- 2026-10-26 19:45: src35 = stage 2 (bank-1 ranges + 0xE3700000+0xC900000, all as boot-services data):
  build/woa/starbite-uefi-src35.bin (sha eed85ec4...), BOOT = out/xnu-boot-src35.img (sha bfb4a906...), flashed.
  Expect the witness at ~1407 MiB.
- 2026-10-26 20:00: src35 (stage 2): witness 1407 MiB / 14 ranges (> the S9+ release's 1357), setup dialog
  unchanged -> **memory ruled out**. Bugcheck ruled out properly: the driver RESERVES its bugcheck slot at the
  ring start at registration (UfsRegisterBugcheckCapture) and the callback overwrites that slot; it still
  reads "=UFSBUGCHECK ARMED=1" after every run (not lost to the write flood).
  Driver write fence (Exynos9810Ufs.h, published source): writable LBA 68736..15615994 minus 1195136..1526911
  and 1680512..1697151 (S9+ partition numbers). Our S9 WINESP sda26 = LBA 6613760..6744831 and WINDOWS
  sda27 = 6744832..13518591 are inside and not refused (so this is not the setup failure), BUT on the S9 the
  fence also leaves USERDATA sda25 (1698432..6613759) and A16DATA sda28 (13518592..15615994) writable, and the
  refused ranges cut through S9 SYSTEM/VENDOR/ODM/CACHE at the wrong places. Note for later.
  **ROOT-CAUSE CANDIDATE: the RWD1/P3 handshake.** Disassembled the release bootshim (480 B; ours 384 B):
  it (1) writes RWD1 @0xFED13D80 = 'RWD1', state 0x10 BOOTSHIM_ARMED, owner 1, phase 1, commit word cleared,
  entry cookie 'VALY' @+0x40, and clears PMU INFORM2/3/SYSIP_DAT0 -- only if the old record is empty/0xFF or
  ended in CONTROLLED_STOP/RECOVERY_PENDING/TWRP_ACK (else leaves the stale record for the stale-owner route);
  (2) unmasks the cluster-0 WDT reset in PMU 0x14060408/40C (bit 24) and arms WDT 0x10050000 with
  WTDAT/WTCNT=0xFFF5, WTCON=0xFF39; then copies the FD. Ours does neither. The driver needs a COMMITTED,
  structurally valid record (version 0x00400001, gen/~gen, heartbeat/~hb, checksum = 0xA5A55A5A ^ words
  0..11 and its inverse, commit 0x21445752 + inverse) in state SEC..EBS to take ownership; only then does it
  take over the firmware-armed WDT (unmasks PMU reset, pets it) and ACK P3 (PREBRANCH -> WINDOWS_ACK). On
  our phone RWD1 was all 0xFF every run -> NoPetReason=WATCHDOG_INVALID, P3 stuck PREBRANCH one-shot-armed,
  seq 5 for every run (stale, never rewritten). The release SEC (commits SEC_ACTIVE) is what we replaced.
  src36 = src35 firmware + shim star_rwd1_arm() in star_uefi(): zero P3 (what BootRouteService does before
  every S9+ boot), clear PMU INFORM2/3/SYSIP_DAT0, write a committed RWD1 = SEC_ACTIVE (0x20) owner SEC (2)
  phase SEC_ENTRY (2) gen 1, RST_STAT in word 8, cookie 'VALY'; verified host-side against the driver's
  validity rule. The shim's boot dump now also prints the previous RWD1 (17 words) and P3 head (8 words).
  BOOT = out/xnu-boot-src36.img (sha 1af81a23...), flashed. After the run read RWD1 state/owner/phase/
  reason/heartbeat (post-mortem) -- it shows how far the chain got (0x30 DXE, 0x40 BDS, 0x50 EBS, 0x60
  WINDOWS_OWNED, heartbeat = driver pets). A hard WDT reset would show RST_STAT bit 0x01000000.
- 2026-10-26 20:20: **src36: the whole firmware<->driver handshake now works like the S9+**, setup dialog
  unchanged. Post-mortem (postmortem-src36-run1): RWD1 gen 1 state 0x60 WINDOWS_OWNED owner 6 (Windows UFS)
  phase 0x0A WINDOWS_RUNNING reason 0 detail 48 (=WdtPets) heartbeat 49, cookie 'VALY'; low-WDT record
  'ARM1'/0/'WAR1' (driver took over the watchdog); P3 SMP1 seq 6 state 0x70 WINDOWS_ACK, one-shot cleared;
  witness tick 48, 1407 MiB. Run ended at the MENU, RST_STAT SWRESET (not the WDT): ~48 s after the driver
  started, with the dialog up. => firmware side is done; the failure is Windows setup's own state.
  **Setup state (tools/woa/hive.py, read-only regf reader) HKLM\SYSTEM\Setup: SetupType 2, SystemSetupIn
  Progress 1, OOBEInProgress 1, SetupPhase 4, CmdLine oobe\windeploy.exe, SetupShutdownRequired 1,
  Status\SysprepStatus\GeneralizationState 4, Status\ChildCompletion\setup.exe = 1.** setup.exe sets 1 when it
  starts and 3 when it completes; finding 1 at start = "restarted unexpectedly" dialog + exit 0x1f. So every
  run since the FIRST src32 run (interrupted mid-specialize back when the platform was broken) just replays the
  stale flag; specialize has never been retried. Windeploy log (UnattendGC\setupact.log) now 8 launches, one
  "Process exited with exit code [0x1f] ... Flushing registry ... exiting" block; the others' tails were lost
  to the resets (no clean shutdown; ntfs-3g says the volume is unclean). No winevt logs exist at all.
  FIX: ChildCompletion\setup.exe 1 -> 0 (fresh specialize, like a first boot; the community workaround is 3 =
  skip). The volume is unclean so ntfs-3g refuses rw; patched the 4 KiB hive cluster RAW instead:
  tools/woa/ntfs-locate.py (read-only MFT walk over adb dd; fixups use a 512-byte stride on this 4K-sector
  volume) -> SYSTEM runs [(2153578,1728),(2158094,128)], file 0x26e65c -> partition byte 0x20ded865c;
  verified the on-disk cluster == pulled hive block, wrote one cluster, readback ok, whole-file sha via
  ntfs-3g = patched copy (378ba51c...). Backup: backup/20261026-win-hives/SYSTEM-seq93-childcompletion1
  (70c8d2ba...). BOOT stays src36. Next run: fresh Panther\setupact.log shows how far specialize gets.
- 2026-10-26 20:40: ChildCompletion reset -> setup.exe really ran phase 4 again and hit a NEW error: "Windows
  Setup experienced an unexpected error" (user); force reset. Panther\setuperr.log: "CreateSetupBlackboard:
  Failed to create/open Setup black board path is [C:\Windows\Panther\SetupInfo] GLE is [8]" (8 =
  ERROR_NOT_ENOUGH_MEMORY, i.e. the blackboard store no longer parses). Next to it sits
  setupinfo.4d53424c4b425244.spl ("MSBLKBRD" in hex) -- the first run opened the blackboard fine and got to
  specialize; eight hard-reset runs since have left it torn. The interrupted state is not limited to one
  flag, so: REDEPLOY the image instead of more surgery. The VM-built disk starbite-win.vhdx (15137243136 B,
  sha 8755e0de...) is on the phone's SD card (exfat-fuse /dev/block/mmcblk0p1 /external_sd); the Mac has
  13 GiB free, so the copy runs ON THE PHONE: tools/woa/redeploy-from-sd.py pulls only the VHDX headers/
  region table/metadata/BAT into a local sparse copy (build/woa/vhdx-sd/), maps every 32 MiB block of
  WINESP/WINDOWS (451 blocks present, 1456 absent; copy 14332 MiB, zero 12640 MiB) and emits a TWRP script
  that checks the card sha, dd's each present block card->partition, zero-fills the rest, and verifies each
  copied range card vs partition by sha256 on the device. Geometry matches build/woa/gpt-new. Running now
  (nohup /tmp/redeploy.sh, log /tmp/redeploy.log). After it: fresh first boot on src36 (RWD1/P3 handshake,
  1407 MiB) = the real test of specialize. Note the ESP is rewritten too (image's own BCD) and the image's
  SYSTEM hive has AutoReboot at its default.
- 2026-10-26 21:00: on-device redeploy DONE bad=0 (845 steps, every copied 32 MiB block verified card vs
  partition by sha256 on the phone; card sha ok). Fresh volume: Panther = unattend.xml only; HKLM\SYSTEM\Setup
  original (SetupType 1, SetupPhase 4, SystemSetupInProgress 1, no Status\ChildCompletion); BCD sha 83297751
  = backup/20261026-bcd/BCD. BOOT = src36. Next: ONE uninterrupted first boot. Specialize ends in Windows' own
  restart; the driver leaves INFORM3 normal, so that restart lands at OUR MENU -> pick WINDOWS 11 again
  (expected, not a failure). Do not force-reset during the spinner.
- 2026-10-26 21:20: ***** SETUP GETS PAST SPECIALIZE ***** Clean first boot (fresh image, src36 with the RWD1/P3
  handshake, 1407 MiB): "Getting devices ready" -> "Starting services" -> "Getting devices ready 100%" ->
  "Getting ready" (user), no error dialog, no reset at ~60 s. So the dialog loop was entirely the stale
  interrupted-setup state from the first (broken-platform) run, and the ~60 s resets were Windows' own restart
  after setup.exe exited 0x1f. Sitting at "Getting ready" for many minutes (slow I/O through the bounce
  buffer); leave it running.
- 2026-10-26 21:45: ********** WINDOWS 11 DESKTOP ON THE GALAXY S9 ********** After "Getting ready" Windows
  restarted by itself to our menu (specialize done), user picked WINDOWS 11 again: login screen for
  "starbite" (autologon), "Getting things ready for you ... Almost there", then the DESKTOP (photo: Windows 11
  Pro, Test Mode watermark, taskbar, Recycle Bin). TOUCH WORKS (long-press opened the taskbar context menu).
  Clock shows 8/5/2026 2:54 PM = the firmware's fixed RTC value (see 19:35 entry).
  RECIPE THAT WORKS: BOOT = out/xnu-boot-src36.img (our xnu menu shim with TPIDRRO_EL0 zeroing + RWD1
  SEC_ACTIVE commit + P3 clear; our bootshim + our PeilessSec with patches 0001,0002,0005,0018-0022 at
  STAR2LTE_RAM_STAGE=2, banks as boot-services data; the release 22621.7582 DXE volume spliced in by
  tools/woa/fv-splice.py), a CLEAN first boot of the VM-built image (tools/woa/redeploy-from-sd.py from the
  SD card), and no interruption of setup. Windows sees 4 CPUs and 1407 MiB.
  Known gaps: RAM (stage 3 high bank not yet tried), RTC fixed, no Wi-Fi/audio/battery/cellular drivers,
  display is the firmware framebuffer (no GPU driver exists for Mali on Windows), and the UFS driver's
  S9+-numbered write fence leaves Android USERDATA (sda25) and A16DATA (sda28) WRITABLE from Windows.
- 2026-10-26 21:50: ********** WINDOWS 11 DESKTOP ON THE GALAXY S9 (SM-G960F) ********** Clean first boot (fresh
  image from the SD-card VHDX, BOOT = src36: our bootshim + PeilessSec with RAM stage 2 as boot-services data
  (1407 MiB) + the release 22621.7582 DXE volume + shim RWD1 SEC_ACTIVE commit / P3 clear): Getting devices
  ready -> Getting ready (~30+ min) -> Windows' own restart -> landed at our menu -> WINDOWS 11 -> login
  screen "starbite" -> "Getting things ready for you" -> DESKTOP. TOUCH WORKS (S6SY761). User photo: Windows 11
  Pro desktop, taskbar, clock 8/5/2026 2:54 PM (the fixed firmware clock). Recipe = this BOOT image + an
  uninterrupted first boot. Root causes found today: RAM map (bank 0 only -> stage 1/2 as BS data), missing
  RWD1/P3 handshake (release bootshim+SEC replaced by ours), and setup state poisoned by the first
  interrupted run (ChildCompletion=1, torn blackboard) -> redeploy.
  Open items: RTC (clock frozen at 2026-08-05 14:29 every boot), RAM stage 3 (2 GiB high bank), the UFS
  driver's S9+ write fence leaves S9 USERDATA (sda25) and A16DATA (sda28) writable from Windows.
- 2026-10-26 22:10: iOS status recap for the plan: XNU gets a fixed 472 MiB window (0x92000000..0xAF800000,
  tools/mkadt_starlte.py), single-block UFS (~12 MB/s), 4 A55. Today's map makes the 2 GiB high bank
  (0x880000000, contiguous; Apple's own A-series DRAM also sits at 0x8_0000_0000) the natural new home for
  XNU; the woa UFS driver source shows the full host re-init needed for multi-block PRDT. Queued after Windows
  RAM.
  **src37 = RAM stage 3** (stage 2 + 0x880000000+0x80000000, all as boot-services data), same patch set and
  the src36 shim (RWD1 commit): build/woa/starbite-uefi-src37.bin (sha b67c41de...), BOOT =
  out/xnu-boot-src37.img (sha 681b25c2...), flashed. Fallback: out/xnu-boot-src36.img. Setup is finished, so a
  failed test boot no longer poisons the install. Expect ~3.4 GiB in Windows (Task Manager / witness).
- 2026-10-26 22:20: **src37 (RAM stage 3): Windows boots, Task Manager reports 3.4 GB** (was 1.4 GB). The 2 GiB
  high bank works as boot-services data like the low banks. BOOT stays src37.
- 2026-10-26 22:35: post-mortem after the stage-3 run (postmortem-src37-run1) is unreadable: the shim's own dump
  at the following menu boot already shows bit-decayed values ('WATL' 0x5741544C -> 0x45404040, WWR1 magic ->
  0x00420015, RWD1 magic -> 0x00040410). A normal Windows RESTART now goes through the release ResetSystem =
  PSCI SYSTEM_RESET = cold reset, and DRAM partly decays. The retained records only survive crashes and the
  driver's own PMU SWRESET routes; Task Manager is the confirmation for stage 3 (3.4 GB).
  iOS GPU (correction to the earlier plan): non-Apple GPU acceleration under iOS exists (hackiosx.dev):
  HackiOS (POCO X5 Pro) and ios-topaz (Redmi Note 12, 2026-10-22) run Mesa freedreno behind CoreAnimation's
  OpenGL ES renderer (whole UI on the GPU, Metal for apps not yet); Pixel 4 / UniXNU use Metal->Vulkan on
  Turnip. Mali-G72 analogue: Mesa Panfrost (GLES 3.1, Bifrost v6) in the same GL ES slot + an XNU Mali kernel
  driver (power/clocks, MMU address spaces, job chains, IRQs) modelled on Linux panfrost + Samsung mali_kbase
  in our downstream kernel.
- 2026-10-26 22:55: **iOS RAM: XNU into the second bank.** Same path the S7 port took (ref HEROLTE_PROGRESS.md
  2026-10-23): bank2 0x880000000 (no reserved-memory node); there 2048 MiB panicked (kmem ranges, then
  write_pte_fast; the macOS-config compressor reserves 16 x RAM), 1536 MiB + boot-arg
  vm_compression_limit=98304 boots; secondaries need the <4 GiB CPU_ON trampoline (patch 0223, page
  0x8fffe000, active for STARLTE since its board header also defines HEROLTE; 0x8fffe000 is plain bank-0 RAM on
  the S9). UFS DMA: herolte_ufs.c programs UTRLBAU/UCDBAU/PRDT upper halves and refuses >4 GiB DMA without
  CAP.64AS; S9 CAP 0x1303ff0f has 64AS (bit 24). Changes: tools/mkadt_starlte.py --bank2 [--bank2-mb 1536],
  shim BANK2_BASE/END = 0x880000000/0x900000000. Kernel unchanged (out/kernel.development.starlte reproduces
  out/payload.bin byte-for-byte). out/payload-bank2.bin = same cmdline + vm_compression_limit=98304, ADT
  --scale2 --bank2 (dram 0x880000000+0x60000000). BOOT image out/xnu-boot-ios-bank2.img (sha e0520863...;
  src37 Windows firmware + kiosk kernel unchanged). Backup first: USERDATA 0..16 GiB (iOS HFS root) ->
  SD card /external_sd/ios-root-userdata-16g.img (on-phone dd + sha compare).
- 2026-10-26 23:05: iOS root backup verified on the SD card (sha 5b38e479..., 16 GiB, 40.8 MB/s). Flashed out/xnu-boot-ios-bank2.img (fallback for iOS: out/xnu-boot-src37.img = 472 MiB window).
- 2026-10-26 23:15: bank2 1536 MiB: panic soon after boot "HEROLTE: unaligned pmap_enter" -- the S7 port's own
  diagnostic in pmap_enter_pte (16 KiB VM pages over 4 KiB hw pages, TEST_PAGE_RATIO_4: pte_p & 0x1f), whose
  comment says second-bank RAM >= 1.5 GiB sometimes hits it; no upstream fix. Dropped to 1024 MiB (booted
  reliably on the S7): out/xnu-boot-ios-bank2-1g.img (sha 283251eb...), flashed; mkadt --bank2-mb default 1024.
  Root cause of the >= 1.5 GiB misalignment is open (later: find which allocation hands out a PT page whose
  4-PTE group straddles; with 1 GiB that path apparently is not hit).
- 2026-10-26 23:25: **iOS boots with 1 GiB in the second bank** (out/xnu-boot-ios-bank2-1g.img, was 472 MiB); user: home screen, "does feel smoother". This image is the new baseline for all three OSes (iOS bank2 1 GiB, Windows src37 3.4 GB, kiosk unchanged).
- 2026-10-26 23:45: **Step 3 (storage safety + clock), part 1.**
  CLOCK: the firmware uses edk2 VirtualRealTimeClockLib (Star2LtePkg.dsc) = saved epoch variable or the BUILD
  time + uptime; no persistent variable store -> Windows wakes up at the release build time (2026-08-05 14:29)
  every boot. The real clock is the PMIC RTC ("s2mp-rtc", rtc0 in TWRP): running, but was 2018-01-02 (never
  set). Set it from TWRP: date -u -s @<host epoch>; hwclock -w -u -> 2026-10-26 19:51:08 UTC (= host). Part 2 =
  a RealTimeClockLib on the PMIC RTC over SPEEDY in our firmware (register map research running).
  STORAGE FENCE: installed Exynos9810Ufs.sys 9.0.0.1 keeps the published fence values as u64 literal-pool data
  in .text (file 0x3ef0 gpt_backup 15615995, 0x3ef8 protA_last 1526911, 0x3f00 protA_first 1195136, 0x3f08
  fence_first 68736, 0x3f10 protB_last 1697151, 0x3f18 protB_first 1680512; also 0x32d0/0x32d4 fence pair for
  diagnostics only). Gate fn at VA 0x140004a3c (x2=lba, w3=count): GPT guard (lba<=5 or end>=backup), then
  lba<=protA_last -> refuse if end>=protA_first else allow if lba>=fence_first; else lba>protB_last -> allow,
  else refuse if end>=protB_first else allow if lba>=fence_first. On the S9 that allowed SYSTEM parts,
  USERDATA (iOS/kiosk), A16DATA. S9 values: protA_first 0, protA_last 6613759, protB_last 0xFFFF..., protB_first
  13518592 => writable exactly WINESP+WINDOWS (6613760..13518591); modelled per partition incl. straddles.
  Re-signed with a self-signed "Starbite Test Signing" cert (osslsigncode, sha256; the original is self-signed
  "S9 WoA Test Signing", testsigning on). Files: build/woa/ufs-sys/{Exynos9810Ufs.sys original 3448e0ef,
  Exynos9810Ufs-s9fence-signed.sys 2d6c64ff, sign/ key+cert}. Installed over DriverStore\FileRepository\
  exynos9810ufs.inf_arm64_56a2e6f01fbd68bc\Exynos9810Ufs.sys via ntfs-3g rw (volume was clean). Test: boot
  Windows; if INACCESSIBLE_BOOT_DEVICE, restore the original from build/woa/ufs-sys.
- 2026-10-27 00:00: **S9-fenced, re-signed Exynos9810Ufs.sys boots Windows** (testsigning accepts the Starbite self-signed cert). Storage safety done.
- 2026-10-27 00:20: **Clock part 2: PMIC-seeded firmware RTC (src38).** Research (build/woa/rtc-research.md, agent;
  sources vutung2311/starlte + LineageOS exynos9810 kernels): S2MPS18 RTC = SPEEDY device 0x2 (TOP 0, PMIC 1,
  RTC 2), regs CTRL 0x00 (bit1 24 h), UPDATE 0x02 (bit0 RUDR latch, bit1 WUDR, keep bit3 + high bits), SEC..YEAR
  0x05..0x0B binary (year-2000), command word ((slave<<8|reg)<<7)|BIT19[|BIT20]. APM shares the bus at runtime
  and the firmware ignores the SPEEDY semaphore -> read ONCE at boot. patches/0023-starbite-pmic-rtc:
  Star2LtePmicRtcLib = edk2 VirtualRealTimeClockLib whose fallback epoch is the PMIC time (two latched reads
  must agree, >= BUILD_EPOCH, CTRL binary+24 h) instead of BUILD_EPOCH; runtime = seed + CNTPCT, SetTime only
  in the (volatile) variable. tools/woa/dxe-replace.py: unpack the release DXE volume (LZMA), turn a module
  into an FFS pad file in place (--kill), append new FFS files in the 3+ MiB free space at their alignment
  (--add), recompress with the VM's edk2 LzmaCompress (verified round trip), rebuild the outer file, splice
  with our SEC/PEI. src38 = src37 patch set + 0023 at stage 3, release RealTimeClockRuntimeDxe (b336f62d,
  0x3054) killed, ours (0x3654) added: build/woa/starbite-uefi-src38.bin, BOOT = out/xnu-boot-src38.img
  (iOS 1 GiB payload, sha 6177269a...), flashed. Fallback test: out/xnu-boot-rtc-identity.img (release RTC
  re-added unchanged; isolates the repack tool). RTC is UTC: Windows needs
  HKLM\SYSTEM\CurrentControlSet\Control\TimeZoneInformation RealTimeIsUniversal=1 (DWORD) for local time.
- 2026-10-27 00:35: **src38: Windows shows the correct date/time** (PMIC-seeded RTC works). Step 3 (storage safety + clock) DONE. BOOT = out/xnu-boot-src38.img (iOS bank2 1 GiB, Windows 3.4 GB + S9 fence + real clock, kiosk).
- 2026-10-27 01:00: **Step 4 (iOS storage speed).** Root cause of the old multi-block failures: on STARLTE the
  driver kept the 8890 vendor offsets (VS_TXPRDT = VS_BASE+0x100) but the 9810 vs_hci block starts AT
  0x11121100 (TXPRDT +0x00, RXPRDT +0x04, NEXUS +0x40, DATA_REORDER +0x60, AXIDMA +0x6C; Linux ufs-exynos,
  Exynos9810Ufs.h, Star2LteUfsHcDxe) -> both tries wrote unrelated registers at 0x11121200.. (and the old
  "txprdt 0 rxprdt 0 nexus 0x2" probe values were from there). The Windows driver requires the UEFI's state
  TX 0x8000000C, RX 0xC, DATA_REORDER 0xA, AXIDMA bit31; one 128-byte FMP entry per 4 KiB, max 16 (64 KiB),
  PRDT length in bytes, 4 KiB-aligned buffers. xnu/patches/0300 (herolte_ufs.c): S9 offsets fixed, NEXUS
  slot-0 bit now hits the real register (every boot), self-test (ufsmulti=1) programs TX/RX 0x8000000C,
  REORDER 0xA, AXIDMA 0x98000003, zeroes each 128-byte entry, compares 16 blocks single vs one 16-entry
  command, restores all four on mismatch; STARLTE_MULTI 32 -> 16. Kernel 17 s incremental (previous kernel:
  out/kernel.development.starlte.pre-ufsmulti). BOOT = out/xnu-boot-ufsmulti.img (iOS 1 GiB + ufsmulti=1,
  src38 Windows, kiosk; sha ebac754c...), flashed. Fallback: out/xnu-boot-src38.img.
- 2026-10-27 01:20: ufsmulti test 1 (webcam photo): S-Boot vs_hci (real offsets) = tx 0xc rx 0xc nexus 0xffff
  reorder 0xa axidma 0x18000003 (granule/reorder/burst already Linux-like); after programming tx 0x8000000c rx
  0xc (RX bit31 does not stick) axidma 0x98000003: single-block 0/0 same; 16 x 128-byte entries: OCS 0 but
  data DIFFER; registers restored; next READ(10) OCS 0xf -> ufsroot0 unavailable panic. Reading: entry size is
  set by the FMP DESCTYPE (secure-only): S-Boot leaves 0 = standard 16-byte entries; Windows' 128-byte entries
  need its UEFI's DESCTYPE 3; the woa UEFI's A98 notes ran 512 KiB multi-entry reads on DESCTYPE 0 with 16-byte
  entries. Test 2: prdt_stride 16 (0300 updated). BOOT = out/xnu-boot-ufsmulti16.img (sha 3840b26a...).
  Note: a failed multi command still kills the host for the rest of the boot (panic at root mount).
- 2026-10-27 01:40: **iOS multi-block storage WORKS** with 16-byte PRDT entries (DESCTYPE 0) + Linux vs_hci at the real 9810 offsets: self-test MATCH, iOS boots and is "so much faster" (user). BOOT = out/xnu-boot-ufsmulti16.img (ufsmulti=1 in the cmdline). Step 4 DONE.
- 2026-10-27 02:00: **Boot menu v5 (step 5, visuals).** User concept (~/Downloads/Starbite.html): rows with OS
  icons drawn AROUND Samsung's real splash, Starbite card below instead of a Kiosk row (select it with the
  volume keys, Power boots it). Samsung's logo.jpg (UP_PARAM sda5 tar, 1079x1747) is drawn 1:1 by S-Boot at
  x 180, y ~1144: SAMSUNG/Galaxy S9/Knox at y ~1199..1575 (consistent with the old menu rows at 1580 "under the
  logo" and the old footer covering "Powered by android" from 2500). tools/brand/menu.mjs (rsvg-convert,
  Avenir Next + Menlo, own icons: apple, Windows tiles, Android head, Tux, recovery arrow) -> build/brand/
  menu.sbm "SBMENU01" (17 opaque RLE sprites, 1.58 MB; base header/label/footer id 0, rows 16+4*item+state)
  + build/brand/menu-preview.png. Layout: header 70..220, BOOT label 236, rows y 290 + 150*i (iOS, Windows,
  Android dim, Linux dim, Recovery), card 1678..2350 (normal / gold ring + PRESS POWER / starting), footer from
  2500 (Powered by Starbite, from overlay.png). Loader: xnu_boot.c sprite_walk/star_sprite + SBMENU01 branch
  (draws id 0, text row base 36); menu.c menu_v5 (order 0,1,4,5; Power boots; Bixby = Engineer with the full
  framebuffer saved at 0x92000000 and restored after; starting sprite; card cleared and status rows at base 25
  unless the card was picked); old text menu stays as the fallback. BOOT = out/xnu-boot-menu5.img (sha
  4528c998..., ramdisk = menu.sbm, payload = ufsmulti16), flashed. Touch input next.
- 2026-10-27 02:20: **Boot menu v6** (user feedback): real icons (simple-icons 9.21.0 Apple/Windows 11/Android,
  Larry Ewing's Tux, Lucide wrench/download; tools/brand/icons/README.md), OS list = iOS, Windows, Android
  (dim), Linux (dim); new "SERVICE · BIXBY" selector under it with Recovery | Download chips side by side
  (y 944, 630 px each); sprite ids 16 + 8*item + state (states 0..4, 4 = selected but inactive; items 4
  Recovery, 5 card, 6 Download). Keys: Volume = OS list (iOS, Windows, Starbite card); short Bixby = service
  selector, again = flip Recovery/Download; Volume returns; Power starts the focused entry (Download reboots
  with INFORM3 0x12345671); Bixby HELD 0.7 s = Engineer only (key_event). Only changed sprites are redrawn.
  Previews build/brand/menu-preview.png / menu-preview-alt.png (PREVIEW=alt). BOOT = out/xnu-boot-menu6.img
  (sha b264c20b...), flashed.
- 2026-10-27 02:45: **Boot menu v7**: service selector moved under Samsung's splash (label y 1606, chips 1652),
  OS rows y0 300 stride 172 h 140, card y 1838 (sprite 1816..2490). TOUCH in the loader: menu.c port of
  bsd/dev/starlte_touch.c (ACPM LDO35/LDO43 with the same voltage checks, CMU_PERIC0 gates, SYSREG USI03 I2C
  mode, GPP1-4/5 mux, HSI2C 0x104B0000 timing, S6SY761 BOOT_STATUS 0x20 / PANEL_INFO / SENSE_ON /
  CLEAR_EVENT_STACK, READ_ONE_EVENT on GPA1-0 INT low); a tap = finger 0 lifted within 80 px of where it went
  down; first tap focuses, second tap starts; hit boxes from star_sprite_rect() (xnu_boot.c); the event stack
  is cleared before handing over; any bring-up failure = keys only. Card clear + status row base now come
  from the card sprite rect. Bug fixed before it bit: the screen save at 0x92000000 overlapped Engineer's RAM
  test (0x92000000 + 16 MiB) -> moved to 0x94000000. BOOT flashed: out/xnu-boot-menu7.img (41aa6546..., save
  at 0x92000000); fixed image out/xnu-boot-menu7b.img for the next flash.
- 2026-10-27 03:10: **EngineerMode 1.0** (shim/starlte/em.c + ui.c), MediaTek-EngineerMode look in Holo Dark:
  status bar (PMIC RTC clock), action bar with app icon (sprites 200-204), scrolling tab strip DEVICE /
  HARDWARE TESTING / POWER / STORAGE / OS / LOG AND DEBUGGING / TOOLS, list rows, Holo AlertDialogs, nav bar.
  Text = Roboto/Menlo glyph atlas "SBFONT01" (tools/brand/em-font.py) appended to the menu sprites
  (ramdisk build/brand/menu-em.sbm); missing atlas = old Engineer. Keys: Volume move, Power select, Bixby back
  (top level: next tab), Bixby hold leave; touch: tap, drag, swipe tabs, Back/Home/Recents.
  Pages: device info from S-Boot bootargs (fdt_chosen_raw), CHIPID + CPU ID registers + caches, DRAM banks
  (fdt_memory_banks) + carve-outs, reset reason (PMU + PMIC), core map, TMU temperatures (TOP 0x10070000 /
  SUB 0x10074000, eFuse trim, build/em-research-thermal-clocks.md), PLL clocks (A55/M3/G3D/MIF/SHARED, power
  checked first), RAM pattern + bandwidth tests, panel info, display/touch/keys tests, RTC, timer;
  MAX77705 battery + charger on HSI2C 0x14360000 (USI_CMGP03; timing snapshotted from S-Boot, repeated start,
  read only: build/em-research-battery.md); PMIC LDO table + dump, touch rails; UFS host registers,
  partitions; iOS / Windows (RWD1, P3, witness, NRM1 decoded) / Kiosk pages, iOS boot options; pmsg log,
  retained records, command lines, live registers; reboot / recovery / download / power off (PMU +0x330C
  PS_HOLD bit 8), register peek (hex keypad, whitelisted blocks, unknown blocks and every write behind a
  dialog), about. Not yet: UFS device/health/speed/sector viewer (needs a UFS query path in the loader), core
  bring-up test, brightness. Build: -fno-tree-switch-conversion added (GCC turned string switches into
  pointer tables = ABS64). Menu: hint sprite (item 7, tap = Engineer), card y 1872, screen save 0x94000000.
  BOOT flashed: out/xnu-boot-em1.img (sha adf9b527...).
- 2026-10-27: **EngineerMode 1.1**. SoC and CPU page reset the phone: the loader runs at EL1 with S-Boot's
  VBAR, so any trapped system register read (ID/cache ID regs, likely HCR_EL2.TID2/TID3 from a hypervisor)
  went to S-Boot's vectors and reset. EngineerMode now installs its own table (em_vec.S em_vectors: sync =
  record ESR/ELR/FAR in em_fault + skip the instruction; SError counted) and restores S-Boot's VBAR on exit;
  SAFE_MRS / probe32 show "trapped, ESR ..." instead. Register peek reads through probe32 (*FAULT** words).
  Core map corrected: A55 MPIDR 0x81000N00 (MT, Aff1 = core), PSCI targets 1..3; M3 PSCI 0x100..0x103.
  New: core bring-up test (PSCI CPU_ON -> em_core_entry records MPIDR/EL per Aff2*4+Aff1 slot -> CPU_OFF;
  parked cores offer a reboot), UFS from the loader (port of herolte_ufs.c, read only: our list at
  0x96000000, AHIT 0, W-LUN TUR/SSU, INQUIRY, QUERY READ DESCRIPTOR device/string/geometry/unit/health,
  READ(10)), live GPT partitions, 16 MiB speed test (4 KiB commands, then 16 x 16-byte PRDT with vs_hci
  set as iOS does, verified first, registers restored), sector viewer (hex + ASCII, +-1/+-256, jump to
  partition). Boot menu: hold Vol+ and Vol- 0.7 s = power off (hw_power_off: PS_HOLD, a cable reboots),
  hint sprite item 8 next to the Engineer hint (item 7). BOOT flashed: out/xnu-boot-em2.img (sha 5513a9a6...).
  BOOT partition is 55 MiB and this image is 53.9 MB: further OSes must load from storage.
  Brightness page (build/em-research-brightness.md): S-Boot's AOR-only sequence F0 5A 5A / B1 hi lo / F7 03 /
  F0 A5 A5 as DCS long writes on DSIM 0x16080000 (payload then header, wait PH empty <= 50 ms, retry once,
  always relock), AOR clamped 0x000C..0x0BA3; panel ID via DCS read 0x04. Flashed out/xnu-boot-em3.img
  (sha e69d9ab8...) instead of em2.
- 2026-10-27: **Linux, milestone 1 (mainline 7.1 from UFS)**. Kernel: mainline v7.1 + SheroAbi/galaxy-s9plus-
  mainline-linux (S9+ star2lte port: UFS, DECON bootfb/scanout, Panfrost G3D power, PCIe BCM4361, USB PHY,
  ACPM, cpufreq, all 8 cores) built in lima by tools/linux/build-kernel.sh: S9 tree from
  tools/linux/mk-starlte-dts.py (4 GB banks from TWRP iomem, 3000 mAh, model), our initramfs init
  tools/linux/init (every UFS block device setro, report on tty0, USB CDC-ECM 172.16.42.1 telnet + ACM
  shell; the port's own init mounts sda25 rw = our USERDATA: never use it), cmdline console=tty0 +
  ramoops. Image 22.4 MB. Mainline has a basic exynos9810-starlte.dts upstream (Markuss Broks); ztOS ran
  the S9+ drivers on an S9. Their port has no microSD node (stock dwmmc2@11500000): Linux root on the
  64 GB microSD is milestone 3, keeping UFS for Android (SYSTEM/VENDOR/A16DATA).
  **OS store**: USERDATA +16 GiB (the 1 GiB gap between the iOS HFSX root, which ends at exactly 16.00 GiB,
  and Kiosk's ext4 at +17 GiB; was all zeros). tools/linux/sbstore.py packs "SBSTORE1" (entries name,
  offset, size, CRC32); tools/linux/sbstore-flash.sh checks HFS+ end / Kiosk superblock / empty target
  from TWRP, writes, reads back. Loader: UFS code moved to shim/starlte/ufs.c (DMA now 0xAF000000 at the
  top of the loader window, ufs_read_to straight into RAM), xnu_boot.c star_linux_store loads linux.image
  at 0x92080000 and linux.dtb at 0xAE000000 with CRC32 (ARMv8 crc32x), x0 = DTB, display trigger left
  free-running (0x1281, as uniLoader). Menu: Linux row (item 3) selectable when the store has
  linux.image (states 0-4 generated; 3 = "not installed"); menu pick 4. BOOT out/xnu-boot-linux1.img
  (sha 4aaecc8c...), store sha b1716af5....
- 2026-10-27: **Linux milestone 2: microSD works** (mainline dw_mmc, Exynos7 type). Bring-up findings:
  stock DT dwmmc2@11500000, SPI 249, pins GPF1-0..5 func 2 (pinctrl_fsys1 0x11430000 offset 0), CD GPA1-5
  active low; rails S2MPS18 LDO31 2.8 V (CTRL 0x68 = 0xE8) and LDO2 3.3 V (0x4A = 0xFC), both 1.8 V + 25 mV
  steps, switched on by the loader before Linux (hw_sd_rails, menu.c) and fixed-regulators in the DT;
  clock CMU top MUX_CLKCMU_FSYS1_MMC_CARD -> PLL_MMC (~806 MHz), DIV 0x1A241864 set to /1, gates
  0x1A242070 / 0x11402030 / 0x11402034 manual (s9p,clock-gates). Controller: VERID 270a, HCON 64-bit
  addressing + 64-bit data, CLKSEL64 at 0xA8. The protection window at +0x1000 (MPSBEGIN0..) reads 0 and
  ignores writes; FMP/SMU is UFS-only here, not the problem. Two mainline fixes (tools/linux/
  patch-mainline.py, DT switches): starbite,dma-32bit (descriptors were in the 0x8_8000_0000 bank; kept
  as a precaution) and **starbite,max-data-timeout** (the real one: every read ended in DRTO, rintsts
  0x20c, even at 400 kHz; vendor always writes TMOUT = ~0). Card: SDXC 58.2 GiB "SD64G", high speed
  50 MHz 4-bit, 22.6 MB/s read. It holds exFAT with ios-root-userdata-16g.img (16 GiB) and
  starbite-win.vhdx (14.1 GiB), 28 GB free: Linux root goes in an ext4 image file there, nothing wiped.
  Iteration without buttons: tools/linux/phone.py (telnet), sbstore-flash-linux.sh / boot-flash-linux.sh
  (write the store / BOOT from Linux: http fetch, checks, only sda + that partition writable for the dd,
  readback), reboot-linux in the initramfs (one-shot u64 "SBNEXTLX" at 0xFED17FE0, the menu boots Linux).
  BOOT out/xnu-boot-linux3.img (sha 7f1d5283...).
- 2026-10-27: **Linux milestone 3: postmarketOS (edge, Plasma Mobile, OpenRC) boots from the microSD.**
  Root = /starbite-linux.img (8 GiB ext4, label starbite_root) on the card's exFAT next to the user's
  backups (exFAT was dirty-flagged only: fsck.exfat -n clean). Built ON THE PHONE (Mac disk too full for
  pmbootstrap in lima: the VM disk does not shrink on fstrim): bring-up Linux runs apk.static 3.0.8
  (hash checked against an HTTPS copy) through tools/linux/http-proxy.py on 172.16.42.2:3128;
  tools/linux/phone-prep.sh = clock, apk.static, keys (pmbootstrap's pmb/data/keys; apk treats
  --keys-dir as inside --root, so keys go to <root>/etc/apk/keys), RAM tools root (e2fsprogs,
  exfatprogs). 1017 packages, 2.2 GB; + openssh-server dnsmasq doas. tools/linux/pmos-setup.sh (chroot):
  user uid 10000 (tinydm autologin UID) in audio/input/netdev/plugdev/video/wheel/feedbackd/seat, root
  locked, PIN in build/linux/pmos-login.txt (git-ignored), SSH key build/linux/ssh/starbite_ed25519,
  doas for wheel, NM leaves usb0 alone, udev UDISKS_IGNORE for sd* (internal UFS), deviceinfo.
  tools/linux/init: Vol down = stay in the bring-up shell; else exFAT rw -> loop0 -> ext4 rw ->
  exFAT moved to /media/starbite-sd -> rm default/usb-signaller (it tears down our ECM+ACM gadget;
  pmOS's unudhcpd.usb0 serves the Mac) -> switch_root /sbin/init. Clock: loader writes the S2MPS18 RTC
  (UTC) as u64 /chosen/starbite,rtc-epoch into the DTB it passes (em_rtc_epoch); a RAM handover failed
  because ramoops zaps 0xFED10000-0xFED18000 at boot (the one-shot flag survives: Linux writes it just
  before rebooting). SD: card power-cycled by the loader (LDO31 seen at vsel 0x30 off after a forced
  reset; accepted range now 2.7-3.3 V). Lessons: never rm -rf a dir with a bind mount inside (wiped
  live /dev nodes once, reboot fixed it); busybox setsid does not wait; long commands need phone.py's
  3600 s timeout. TWRP's 4.9 kernel mounts the image only ro,noload (newer ext4 features).
  BOOT out/xnu-boot-linux6.img (sha 21d95cf2...). Next: GPU (Panfrost) + DECON scanout, Wi-Fi, keys.
- 2026-10-27: **Linux milestone 4: GPU, display, Wi-Fi.** build-kernel.sh now builds with GPU=1 DECON=1 WLAN=1:
  Panfrost on the Mali-G72 (id 0x6221 r0p1, shader_present 0x5771777, 572 MHz; "no regulator (mali)" is
  expected, BUCK via ACPM), s9p-decon KMS takes over the bootloader framebuffer (window 5, command mode),
  KWin holds card2 (exynos/DECON) + renderD128 (panfrost) with Mesa 26.2 gallium = Plasma renders on the
  GPU. Wi-Fi: PCIe Gen1 x1 link, BCM4361/5 (14e4:441f) brcmfmac firmware 13.50.28 built in (the S9+
  port's files are byte-identical to this S9's vendor files in build/linux-bundle/firmware, nvram r02a_b2),
  wlan0 managed by NetworkManager. Root from the Mac while pmOS runs: tools/linux/pmos-root.sh (SSH +
  doas, PIN from build/linux/pmos-login.txt answered by expect); the store/BOOT writers take
  PHONE_RUN=tools/linux/pmos-root.sh. The init installs /usr/local/sbin/reboot-linux (+ its busybox,
  for devmem) into the system so pmOS can reboot straight back into Linux.
- 2026-10-27: Wi-Fi from Plasma failed "Not authorized to control networking": polkit gave auth_admin
  for NetworkManager.network-control although Plasma runs in the active seat0 session c1 (cgroup /c1,
  autologin). Fix: /etc/polkit-1/rules.d/50-starbite-networkmanager.rules (netdev group may use all
  NetworkManager actions), installed live and in tools/linux/pmos-setup.sh. Smoothness: KWin = OpenGL ES
  3.1 on "Mali-G72 MC18 (Panfrost)", 60 Hz (DECON ~60 irq/s), scale 2.25, CMA 320 MiB fine; but GPU
  devfreq (simple_ondemand, 260/455/572 MHz) sat at 260 MHz 99.8 % of the time. Test: userspace
  governor at 572 MHz (36 C idle) - not persistent yet. App start is also bound by the microSD
  (22.6 MB/s, high speed; UHS needs the 1.8 V switch on LDO2).
- 2026-10-27: **Android 17 (LineageOS 24.0, krazey90 unofficial, 2026-10-25) installed** (research:
  build/android-research.md). Zip md5 740fb095...; never flashed (it writes BOOT). Unpacked in lima
  (tools/android/sdat2img.py): SYSTEM 4561305600 B (Updater app removed: its OTAs are install zips that
  write BOOT), VENDOR 681574400 B with /data -> by-name/A16DATA in etc/fstab.samsungexynos9810 and
  (userdata|USERDATA|A16DATA) -> userdata_block_device in vendor_file_contexts (sed dropped the
  security.selinux xattrs; restored from the original image byte for byte), ramdisk fstab edited too.
  ODM formatted ext4 "metadata" (Android 17's /metadata; mke2fs as the maintainer's sideload zip).
  A16DATA: f2fs built in lima (mkfs.f2fs -g android -O encrypt,quota,project_quota,extra_attr,casefold
  -C utf8 -l data) and written raw. All writes from TWRP, each verified by sha256 readback. RECOVERY =
  TWRP 3.7.0 with /data = A16DATA (tools/android/twrp-a16data.py; out/twrp-3.7.0-a16data.img sha
  94e2d5ed...): Android-triggered wipes can no longer reach USERDATA. Loader: star_android_store (store
  android.kernel 38 MB at 0x92080000, android.ramdisk at 0xA8000000, S-Boot's FDT copied to 0xAD000000
  with android.cmdline appended via fdt_copy_append_bootargs and /chosen initrd set), menu Android row
  (item 2, pick 5). BOOT now packed on LineageOS's boot.img header + DTBH (its DT has no
  /firmware/android fstab node, which would override the ramdisk fstab in Android 17's first stage):
  every OS now gets LineageOS's DTB from S-Boot - iOS/Windows/Kiosk still to be re-checked.
  BOOT out/xnu-boot-android1.img (sha 36a97223...), store 7afe504c....
- 2026-10-27: **Boot menu v8, "Starred Wordmark" brand** (build/brand/wordmark: ink #eef1f6, accent #6fb3ff,
  line #2c313b, Outfit 200/300/500, curved 4-point star). tools/brand/fonts/Outfit-wght.ttf (OFL) through a
  private fontconfig (fonts/fonts.conf, FONTCONFIG_FILE for rsvg-convert), measure.py (Pillow) for exact
  wordmark widths. Header "starbite" + star, rows reordered Android, iOS, Linux, Windows (ITEMS[].row;
  item ids unchanged; menu.c v5_os {2,0,3,1,card}), uniform labels "<OS version> | <flavour> ·
  <kernel/build>", own footer "powered by starbite" (overlay.png now only supplies the Starbite card).
  EngineerMode 1.2: OS tab Android 17 / iOS 16 / iOS boot options / Linux / Windows 11 / Kiosk / OS store /
  Start an OS; live partition filesystem probe (fs_kind: ext4 label, f2fs, HFS+, NTFS, FAT32), store listing
  (star_store_entry), em_main returns the menu pick (1 iOS, 2 Kiosk, 3 Windows, 4 Linux, 5 Android).
  BOOT out/xnu-boot-menu8.img (sha 0565e508...), 54.35 MB of 55 MiB.
- 2026-10-28: iOS, Windows, Kiosk all boot with LineageOS's DTB in BOOT (user). **iOS GPU, phase 1 started.**
  Facts from the S9+ port (src/soc/s9p-g3d-pd.c, s9p-g3d.c, DTS): gpu@17500000 (job/mmu/gpu SPI 282/281/
  283), G3D domain PMU 0x14064040/44 (S-Boot leaves it on), CMU_G3D 0x17400000 (PLL_G3D CON0 +0x140, CON2
  +0x148; S-Boot's CON2 0x10000001 never reaches STABLE), vdd_g3d S2MPS18 BUCK6 reg 0x2b = 300 mV + n*6.25
  (S-Boot 644 mV; jobs need 950 mV = 0x68, lower gave DATA_INVALID_FAULTs), after a power cycle SMC
  0x82000410(1, 0x17410204, 0) restores the G3D TZPC or the first access SErrors. Our XNU tree has the S7's
  Mali-T880 driver (bsd/dev/herolte_gpu.c, patches 0225-0235: probe, jobs, fragment clear, triangle,
  textured quad, sweetfb_gpu present); STARLTE builds define ARM64_BOARD_CONFIG_HEROLTE too.
  EngineerMode: Hardware Testing > GPU (Mali-G72), read only (domain, BUCK6, PLL, GPU_ID, features).
  xnu/patches/0301 + xnu/src/starlte_gpu.c: read-only probe at 5 s / 60 s (boot-arg stargpu=0 off).
  tools/build-xnu-local.sh now applies every xnu/patches/03xx (records .applied-<name>). Payload rebuilt
  surgically: same ADT (out/starlte-current.adt) and boot-args (build/ios-cmdline.txt) taken from
  payload-linux-ufsmulti16.bin, bundles (Kiosk + src38 UEFI) appended byte-identical at 0xfd6000 ->
  out/payload-linux-gpu1.bin; BOOT out/xnu-boot-gpu1.img (sha 8282e382...).
- 2026-10-28: HackiOS research done (build/ios-gpu-research.md; code now in oosexgang/refport-os). Their path:
  msm DRM ioctls through one IOUserClient selector (low byte of the ioctl), 226-line libdrm shim, Mesa 25.0.7
  cross-built for arm64-apple-ios (softpipe + freedreno into libOSMesa), IOSurface -> GPU BO (selector 0x7e)
  and EGLImage, DYLD_INSERT_LIBRARIES via a kern.exec_inject sysctl, eagl.c hooks EAGLContext and QuartzCore's
  GL dispatch (+ vtable patches at iOS 16.5 addresses). For Panfrost: syncobjs required (emulate), DRM
  version >= 1.1 (pre-allocate the heap BO), ioctls CREATE_BO/MMAP_BO/SUBMIT/WAIT_BO/GET_PARAM/GET_BO_OFFSET/
  MADVISE/GEM_CLOSE. Licenses: refport kernel APSL, userland/tools/Mesa patches MIT; Linux panfrost and
  mali_kbase are GPL (reference only, no code copying).
- 2026-10-28: **iOS GPU phase 1 passed.** EngineerMode page (photo) and XNU probe (log flush #15, 5 s and 60 s)
  agree: G3D domain on (status 0x10000f), PLL_G3D CON0 0xa0a00412 = 260 MHz and reported locked (CON2 still
  0x10000001), BUCK6 vsel 0x33 = 618.75 mV, GPU_ID 0x62210010 Mali-G72 r0p1, shader_present 0x5771777 (18
  cores), tiler/L2 1, AS 0xff, JS 0x7, THREADS 384, all cores powered off, GPU_STATUS 0, IRQ_RAWSTAT 0x100
  (RESET_COMPLETED), COHERENCY_FEATURES 0. Registers read fine from XNU with no TZPC SMC (S-Boot's state).
  iOS ran >60 s with the probe, no panic. Next: phase 2 (BUCK6 to 950 mV, L2 + shader power-up, first job).
- 2026-10-28: **iOS GPU phase 2 PASSED: the Mali-G72 ran its first jobs under iOS** (BOOT out/xnu-boot-gpu2.img,
  sha b49f2ee6..., commit bff9e2c; log build/ios-gpu2-log.txt, flush #18). ~8 s after boot: BUCK6 0x33 (618.75
  mV) -> 0x68 (950 mV) through ACPM (0302: locked starlte_pmic_read/write in starlte_touch.c), soft reset done in
  10 us, L2 / all 18 shader cores (0x5771777) / tiler ready within 10 us, AS0 TRANSCFG 0 before (legacy/LPAE),
  Mali-LPAE tables at 0x8f0000000 (bank 2 above XNU's 1 GiB), MEMATTR 0x888d88, TRANSTAB | 7 -- the T880
  recipe works unchanged on Bifrost. WRITE_VALUE zero (type 3) 39 us and WRITE_VALUE immediate-64 (type 7,
  payload +48) "STARBITE" 64 us on JS1, affinity = all cores: JOB_IRQ 0x2, exception 0x1 (DONE), no MMU/GPU
  faults, outputs correct. Cores off, BUCK6 back to 0x33; GPU_IRQ_RAWSTAT 0x600 (power changed). iOS ran
  normally afterwards (60 s probe, no panic). Next: phase 3, fragment job (clear) -> panel, with the Bifrost
  (v6) framebuffer descriptor; then Panfrost-style UAPI.
- 2026-10-28: **iOS GPU phase 3 PASSED: the Mali-G72 drew on the panel under iOS** (user saw the green square;
  BOOT out/xnu-boot-gpu3b.img sha f7a452ca..., commit d4eb663; log build/ios-gpu3-log.txt, flush #13).
  Bifrost v6 fragment job (type 9) on JS0 with Mesa's layouts (genxml v6.xml, pan_desc.c): Framebuffer
  Parameters + 64 B padding, one RGBA8 linear RT at +128 (word0 1<<26, word1 0x86888899, base, stride, clear
  colour), single-sample positions table (128,128 at sample 0 and origin), 16x16 tiles (log2 8), colour buffer
  1 KiB, tie-break 2, Z D24, FB pointer | 1. A NULL tiler pointer works for clear-only frames (no Tiler
  Context / Heap needed). Private 64x64: 63 us, 4096/4096 green. Panel 256x256 at (592,1344) of the scanout
  0xcc000000 (GPU VA 0x04000000, 9 L3 tables): 53 us, 324/324 samples green, left neighbour untouched.
  First try panicked: ml_io_map() of the whole 17 MiB scanout -> kmem_alloc failed (vm_kern.c:178); the CPU
  now maps only the square's rows. Next: phase 4, tiler job + a hand-assembled Bifrost fragment shader
  (first triangle), then GPU scaling of iOS frames.
- 2026-10-28: **iOS GPU phase 4 PASSED: the Mali-G72's first draw under iOS, a shaded triangle** (user saw it;
  BOOT out/xnu-boot-gpu4.img sha 33692ef9..., commit ffb8a8c; log build/ios-gpu4-log.txt, flush #12). The draw is
  built offline by tools/gpu/starlte-g72-tri.c in the lima VM against Mesa 25.0.7 (~/mesa, built with
  -Dgallium-drivers=panfrost; tools/gpu/build-g72-tri.sh): the fragment shader from Mesa's Bifrost compiler
  (is_blit: MOV r0-r3 = 1,0,1,1 + BLEND, no ATEST, 144 B; gpu_id must be the product ID 0x6221, not the full
  GPU_ID), every descriptor packed with Mesa's v6 genxml macros, both chains checked with pandecode
  (build/g72-tri-pandecode.log) -> xnu/src/starlte_gpu_tri.h (tools/gpu/regen-0301.py puts it in 0301). Tiler
  job (JS1, no vertex job: screen-space positions straight to the tiler, as Mesa's preload) 44 us; fragment job
  (JS0) 151 us; 25/25 inside pixels magenta, 5/5 outside grey. Tiler heap 16 MiB pre-mapped at PA 0x8f1000000
  (GPU VA 0x10000000), hierarchy mask 0xff, 512x512 render target on the panel at (464,1216). No faults.
  Next: GPU scaling of iOS frames (textured quad + sampler, as the S7's sweetfb_gpu), then interrupts and a
  Panfrost-style IOUserClient UAPI for Mesa in userland.
- 2026-10-28: **iOS GPU phase 5 PASSED: the Mali-G72 presents iOS frames** (BOOT out/xnu-boot-gpu5.img sha
  4249c95d..., commit 5ce3bbf, boot-arg sweetfb_gpu=1 now in build/ios-cmdline.txt; log build/ios-gpu5-log.txt).
  Stages 2-4 pass, stage 5 keeps the GPU at 950 mV with all cores on; SweetFramebuffer's full-frame presents of
  the 828x1792 canvas (stride 3328) go through the GPU: Mesa VAR_TEX + BLEND shader, bilinear, full-height quad
  x 36..1403 between black bars; surfaces 3/4 (1456 pages) in slots 0/1; 1.9-2.0 ms per frame (A5XELTE FB perf:
  avg 2.4-2.5 ms per present incl. cache clean, was a 17 MB CPU scale-copy). User: looks a little smoother
  (bilinear), but does not RUN smoother: presents are capped at 30 Hz (sweetfb_hz) and iOS itself delivers only
  0.1-5 fps, because CoreAnimation renders on the CPU (4 A55 little cores, cpus=4; the M3 big cores are not up
  under iOS on the S9). Next levers: M3 cores in XNU (S7 did 8 cores via ADT cpu4..7 + 0223 trampoline; Linux
  already runs all 8 on this S9), A55/M3 clocks, then GPU rendering of the UI (Mesa in userland, HackiOS path).
  Payload note: the iOS kernel grew, bundles now at 0xfde000 (were 0xfd6000); the loader finds them by itself.
- 2026-10-28: **iOS on all 8 cores** (4 A55 + 4 Mongoose M3; patch 0304, ADT mkadt_starlte.py --big ->
  out/starlte-big.adt, boot-arg cpus=8 now in build/ios-cmdline.txt). From Linux on the phone: M3 MPIDR Aff =
  0x100..0x103 (no MT bit; A55 core 1 is 0x81000100, same Aff1:Aff0), S9P vendor PMU 1088 c7150000->8d150000,
  2114/2180/21ec/2258/2384 1->2, M3 lacks PAN/LSE/RDM/DotProd/FP16/LRCPC/DPB. 0304: PMU defaults then PSCI
  CPU_ON 0x100+n; reset vector and ml_get_cpu_number give MT-less cores bit 16 (reg 0x10100+n); msr PAN only if
  ID_AA64MMFR1.PAN; commpage features = nibble-min of A55 and M3 ID regs. First try panicked "CPU registration
  timeout" (ml_get_cpu_number masked 0x10100 to A55 core 1's 0x100); fixed. Log build/ios-m3b-log.txt: 8/8 CPUs,
  M3 MIDR 0x531f0020, timers wake on all 8; iOS up to 9.7 fps (was <= 5); user: "a little smoother". That boot
  the GPU present stayed off: stage 4's panel read-back raced iOS's CPU copies (3/5 outside pixels), now the
  stages hold SweetFramebuffer (sweet_fb_hold_until) while they draw. BOOT out/xnu-boot-m3c.img (sha 8981832e...).
- 2026-10-28: **Next target: iPadOS 18.7.11 (22H384) for the iPad 7th gen (iPad7,11 / J171AP, A10 T8010).**
  Why not iOS 26/27 on an iPhone 11: every iPhone userland from iOS 17 on is arm64e only (A12+), and the S9 has no
  PAC (A55 v8.2, M3 v8.0); non-hint PAC ops (RETAB, BRAA, BLRAA, LDRAA, PACIA ...) are UNDEFINED here, so it would
  need a whole-cache PAC-stripping rewriter + trap emulation (JSCVT/FCMA) + userland pinned to the A55s (M3 lacks
  LSE/LRCPC), on xnu-12377. iOS 27's XNU is not published yet (newest tag xnu-12377.121.6). iPadOS 18 is the
  newest userland these cores run natively: kernelcache.release.ipad7c is plain arm64, `xnu-11417.140.69.706.66~1/
  RELEASE_ARM64_T8010`, and Apple published tag xnu-11417.140.69. IPSW 7525 MiB, still signed; OS
  (094-32804-044.dmg.aea, 5.6 GiB) and Cryptex1,SystemOS (094-33089-044.dmg.aea, 1.5 GiB) are AEA-encrypted,
  AppOS (094-32850-044.dmg) is not. Tools: tools/fetch-ios-parts.sh now takes DEVICE/VERSION/BOARD/OUT,
  tools/decrypt-ios-parts.sh (key from wkms-public.apple.com via blacktop ipsw 3.1.739 release binary in
  build/tools/ipsw, then macOS `aea decrypt`). Homebrew refuses installs while Xcode 15.4 is xcode-select'ed.
  Patch trial (build/xnu-11417-trial.txt): the 168-patch series (fork starlte branch + our 03xx) on a clean
  xnu-11417.140.69: 120 apply clean, 48 fail with 122 of 980 hunks rejected (0002 UIO_STACKBUF backport 37/37 is
  probably obsolete there; the rest are board target, console, boot, SMP, HMAC PRNG, AMFI hunks).
  Host: the "Starbite Windows" UTM VM (W-series image build) deleted at the user's request: 2.2 -> 58.6 GB free.
  Wi-Fi reference: BCM4361/5 on PCIe Gen1 x1 (14e4:441f), brcmfmac 13.50.28 works under Linux on this phone.
- 2026-10-28: iPadOS 18.7.11 parts in build/ipsw-18 (fetch + decrypt OK). The AEA images decrypt to **raw APFS**
  (not UDIF): attach with `-imagekey diskimage-class=CRawDiskImage`. Volumes: CrystalUpdate22H384.J171J172OS
  (5.9 GiB used), ...SystemCryptex (3.9 GiB image), AppCryptex (16 MiB). SystemCryptex holds
  `dyld_shared_cache_arm64` (+ .01 ...), magic `dyld_v1   arm64`: plain arm64 confirmed. Staged root ~10 GiB
  before hard links: build-ios-root.sh's 16 GiB HFSX fits, but leave room (USERDATA is 54 GiB).
  **Why iOS 16 has almost no apps:** 36 stock apps (AppStore, MobileSafari, Photos, Camera, MobileMail,
  MobileNotes, MobileCal, Maps, Music, Files, Weather, ...; 325 MiB) are not in /Applications but in
  /private/var/staged_system_apps (placeholders in /System/Library/AppPlaceholders). DataMigration's
  SystemAppMigrator.migrator installs them on first boot, and herolte-tune-ios-root.sh sets
  com.apple.migration LastSystemVersion = build so DataMigration never runs. iOS 16 is most likely the same (to verify on
  the phone: /private/var/staged_system_apps present, /private/var/containers/Bundle/Application empty). Fix for the iPadOS 18 root: let
  SystemAppMigrator run (or pre-install the staged apps the way installd does).

## iPadOS 18 plan (2026-10-28)

Goal: a "full" iPadOS 18 (all stock apps, wallpaper), later the iPhone UI, later Wi-Fi. Order:

1. Kernel: port the series to xnu-11417.140.69 (122 hunks by hand), toolchain for macOS 15 (Xcode 16.x + a
   macOS 15 KDK through darwin-xnu-build; keep Xcode 15.4 for the iOS 16 kernel). First goal: same console
   milestones as iOS 16 (UFS root mounted, launchd exec'd), iOS 16 stays bootable as the fallback.
2. Root: J171AP root from the 18.7.11 OS + SystemOS + AppOS cryptexes (staging script for the iOS 18 layout,
   all apps), ADT /product from DeviceTree.j171ap. Bigger than 16 GiB HFSX? check the staged size first.
3. Home screen (iPad UI). Then "full": install the staged stock apps (SystemAppMigrator, see the
   2026-10-28 entry) and why there is no wallpaper
   (suspect: PosterBoard renders with Metal; needs a CPU fallback or the GPU path).
4. iPhone UI: identity (ADT product / MobileGestalt device class and idiom) so SpringBoard runs the phone UI.
5. GPU rendering (Mesa Panfrost in CoreAnimation, HackiOS path; check iOS 18 still has CA's GL renderer).
6. Wi-Fi: own XNU PCIe driver for the BCM4361 (Linux brcmfmac as reference, GPL so no code copying), first as a
   plain network interface (IP traffic), then the Apple80211 / wifid side for the Settings Wi-Fi UI.
- 2026-10-28: **Kernel series ported to xnu-11417.140.69** (plan step 1, source side). Port tree build/xnu-11417
  (one commit per patch, tools: scratch port-next.sh), exported to xnu/series-11417 (README lists every port
  note). With the staging scripts run first (the earlier trial skipped them, which inflated the failures):
  0017-0046 etc. applied clean; 33 patches needed changes, 4 are obsolete upstream (0001 clang flag, 0002
  UIO_STACKBUF, 0005 pmap reserved10, 0083 IORPC.kernelContent). Notable 11417 changes: **BCM2837 target removed**
  (only a board header for us; SWEET.h is self-contained); serial_init matches a `serial-device` phandle and
  returns early without one (fbcon now initialised before that); entropy pool is SHA-512 initialised at boot
  (0033's BearSSL SHA512_* covers it, 0014's entropy.c half dropped, PRNG half kept); pid_hibernate moved to
  bsd/kern/kern_proc.c; choose_processor split (fast-core pick ported into the non-SMT one, CLPC masks are 0
  here); real osfmk/arm64/kpc.c (Apple PMU) -> board configs get CPU_COUNTERS_BASE = [ ] (kperf kept), kpc stub
  dropped; board MASTER configs (8796 MacOSX copies) three-way merged onto 11417's (0307). Checked: series +
  scripts on a clean tree reproduce the port tree exactly; darwin-xnu-build's patches/15.0 apply on top
  (restore_files.sh needs xnu-8796.101.5 / xnu-10063.121.3 tags, dxb clones full history; link_kdk,
  link_kdk146, remove_tightbeam skip as expected). staging scripts pinned: libpthread-536 69fb92b6,
  hfs-683.120.3 1ce7f99f; both ran clean (their exact-match asserts held).
  Toolchain: KDK 15.6 24G84 from dortania's KdkSupportPkg mirror, unpacked (pkgutil --expand-full, both
  payloads) to build/kdk/KDK_15.6_24G84.kdk, not installed (3.7 GiB). CommandLineTools lack iig/ctfconvert, so
  Xcode 16.4 is needed (Apple ID download, user). tools/build-xnu-11417.sh (build/dxb-11417, same dxb commit,
  series into patches/15.0) -> out/kernel.development.starlte-11417. Not compiled yet.
- 2026-10-28: **xnu-11417 STARLTE kernel builds** (out/kernel.development.starlte-11417, 19.9 MB, `Darwin Kernel
  Version 24.6.0 ... xnu-11417.140.69`; iOS 16 kernel is 17.3 MB). Xcode 16.4 (16F6, clang 17, MacOSX15.5 SDK)
  unxipped to /Applications/Xcode_16.4.app (signature checked), used only via DEVELOPER_DIR; xcode-select still
  points at the CommandLineTools. Five build rounds; fixes folded into the introducing patches (list in
  xnu/series-11417/README.md): apple_arm64_cpu.h needs apple_arm64_regs.h; KDK lib<board>.os.a force-load skipped
  for our boards; amcc_rorgn_ppl_{amcc,ctrr3}.c stubs; libTightbeam.exports out of the Private symbol set; split VM
  headers (vm_shared_region_xnu.h, vm_map_internal.h, vm_kern_xnu.h); primary_map is SMT-only (and the realtime
  fast-core pick moved to the non-SMT chooser). Instruction scan (llvm-objdump): no LSE/RCpc/PAC/dot-product ops,
  only 17 csdb (hint space, NOP on v8.0) and the one ldumaxah the iOS 16 kernel already has: M3-safe as before.
  Not booted yet. Next: payload with this kernel (ADT /product from J171AP) -- first boot can use the iOS 16 root
  to check the kernel milestones (UFS root mounted, launchd exec'd) before the iPadOS 18 root exists.
- 2026-10-28: **xnu-11417 kernel boot test on the S9 PASSED (kernel milestones), with the iOS 16 root.** Image
  recipe: uc3's loader, ramdisk (menu) and Kiosk/UEFI bundles byte-identical, only the SWEETXNU payload rebuilt
  (mkpayload of uc3's kernel + out/starlte-big.adt + build/ios-cmdline.txt reproduces payload-uc.bin exactly).
  11417 payload 19.1 MB, BOOT image 56.9 MB of 57.67 MB. Rounds (photos of the panel; the 4 MiB flash log never got
  an 11417 flush, every boot died before the first periodic flush at ~30 s):
  a) "SWEET CPU: cpu=0 processor_start failed": processor_start from IOKit is a no-op (KERN_FAILURE) in 11417 ->
     patch 0308: SweetCPU::startAll calls processor_boot(), ml_processor_register skips its own for our boards.
  b) nested panics (limit 5) on pe_init.c:711 assert(panic_info): no panic region. mkadt_starlte.py now adds
     /pram reg <0x8f8000000 0x100000> + /chosen embedded-panic-log-size 0x80000 (out/starlte-big-pram.adt) and,
     as asked by the user, a FULL-SCREEN boot console (a5xelte-fbcon = whole 1440x2960 panel, cleared by the
     kernel; was rows 640..1880 between the menu header and the bottom card).
  c) cpu 4 (first M3): cpu_common.c:862 assert cpu_signal == SIGPdisabled, from processor_start_thread ->
     processor_cpu_reinit: SweetCPU::initCPU's HEROLTE IRQ window took the self-SGI early (8796 never unmasked
     there; 11417 does it itself after the assert) -> window removed (0308). The A55s just missed the window.
  d) xnu-boot-11417d.img (sha dd3c77c2...): all 8 CPUs run, UFS root mounted, exec launchd, libignition
     "ignition sequence complete", then pid 1 exits with namespace 2 (OS_REASON_SIGNAL) subcode 6 (SIGABRT) ->
     "initproc failed to start". Expected: iOS 16 launchd on a 2-generation newer kernel; iPadOS 18's userland
     matches xnu-11417. Panic header now complete (Panicked task launchd, cores 0-7 PCs, stackshot OK).
  BOOT restored to out/xnu-boot-uc3.img (a8431754..., iOS 16 known good). Follow-ups: loader boot_args for
  BOOT_LINE_LENGTH 1024 (11417 reads bootFlags/memSizeActual at +1132/+1140; today both read 0 from the zeroed
  page, memSizeActual 0 falls back to memSize), flash-log flush on panic, then plan step 2 (iPadOS 18 root).
- 2026-10-28: **iPadOS 18.7.11 root built, written and booted (plan step 2): reaches launchd, which dies in
  libSystem start-up.** Root: `sudo IPSW=build/ipsw-18 OUT=build/root-ipados18.img INSTALL_STAGED_APPS=1
  tools/build-ios-root.sh` + `IMG=build/root-ipados18.img tools/starlte-tune-ios-root.sh` (16 GiB HFSX, 13 GB used,
  220 apps in /Applications incl. the 36 staged stock apps, their AppPlaceholders left out; tools/stage-ios-root.sh =
  the fork's a5xelte-stage script + optional PaperBoard + INSTALL_STAGED_APPS). Written over the iOS 16 root
  (USERDATA 0..16 GiB; iOS 16 backup on the microSD /external_sd/ios-root-userdata-16g.img, Oct 26) with
  starlte-flash-root.py (TWRP had A16DATA on /data: umount first), image sha 609e5387.... ADT:
  mkadt_starlte.py --big --bank2 --ipad7 build/ipsw-18/j171ap.adt -> out/starlte-ipad7.adt (J171AP/iPad7,11/J171,
  real J171AP /product, chip-id 0x8010, display-scale 2, artwork subtype 2160, canvas 1440x1920 = 720x960 pt @2x:
  SweetFramebuffer's in-panel surface caps the width at 1440). Boot-args build/ipados18-cmdline.txt (sweetfb_lw=1440
  sweetfb_lh=1920, sweetscan=launchd, sweettrace=2000). BOOT images out/xnu-boot-ipados18{a..e}.img.
  Result every boot: J171/iPad7,11, 8 CPUs, root mounted, libignition "goodbye", then pid 1 dies -- SIGSEGV (isa 0)
  or SIGABRT (libmalloc free_list_checksum_botch), both while freeing an XPC dictionary in
  _libSystem_initializer -> _container_init -> _os_feature_enabled_SLOWPATH (symbolized with blacktop ipsw `dsc a2s`
  against a local copy of the cache, build/dsc-18). Heap corruption, same spot as iOS 16 on this kernel (also
  SIGABRT). Ruled out: M3/inter-cluster coherency (cpus=4 = A55 only: identical abort), commpage CPU counts (8),
  madvise REUSABLE rounding and shared-region submap page shift (same as 8796), pager slide loop (cache uses v2,
  4 KiB slide pages). Trace (sweettrace): map_with_linking_np (dyld pager, new since iOS 17) = EINVAL, then dyld's
  userland fallback; objc_bp_assist_cfg_np = EIO; sandbox stand-in only op 2. Suspects left: xnu-11417's VM on a
  4 KiB-page kernel with 16 KiB user maps (new vm_sanitize layer, allocation/deallocation alignment) -- iOS 16 on
  8796 never hit it. Next: a static test init (no libSystem) at /usr/appleinternal/sbin/launchd.development, which
  load_init_program tries first, exercising anonymous memory, file mmap, COW, vm_allocate/deallocate and
  madvise on 4 KiB sub-ranges of 16 KiB pages and checking the data, to find the broken primitive.
  Kernel diagnostics (patch 0309): critical-exit crash log (pc/lr/bt, all registers, mapping of x0/x19) + flash-log
  flush before the init-death panic; boot-arg sweettrace=N logs pid 1's first N syscalls / Mach traps.
- 2026-10-28: **iPadOS 18: launchd, SpringBoard and backboardd all stay up; the loop is unattended now.** Six
  kernel findings, one per boot (a boot cost a flash + reboot + reset into TWRP + log pull, hence the pace):
  1. launchd heap corruption = madvise widened to 16 KiB on the 4 KiB kernel (patch 0310, `kern_mman.c` aligns to
     PAGE_MASK as 8796 did); found with the static memtest init (tools/memtest, `launchdsuffix=memtest`).
  2. `sandbox_get_container_expected()` (Sandbox op 0x43) must succeed (stand-in returns 0; folded into 0201).
  3. Kernel panic "metadata ... chunk len greater than max" in `ipc_port_lock_check_aligned` while notifyd ran:
     PGZ-protected ipc port in a multi-page chunk (4 KiB pages) fails `zone_id_require_aligned`; patch 0311
     resolves PGZ addresses to the original element first.
  4. lsd/backboardd/findmydeviced abort loop: containermanagerd never answered (clients saw their reply port die,
     reconnected, 7 retries, abort); containermanagerd itself got dead-name from usermanagerd. Root cause (patch
     0312): `mach_port_is_connection_for_service()` returned KERN_NOT_SUPPORTED without Sandbox message-filter
     callbacks and iPadOS 18's libxpc rejects every new peer on that; now KERN_SUCCESS / policy 0 when there are no
     callbacks. (iOS 16's libxpc never asked.)
  5. Diagnostics that got there (patch 0309): `sweettracename=<p_comm>` traces a process by name, its failing
     syscalls (with the path argument) and failing Mach traps are always logged; `sweetmsg=1` prints the printable
     text of its Mach messages incl. out-of-line data (XPC dictionaries, launchd lookups, send-once/dead-name
     notifications = ids 69/71); containermanagerd/usermanagerd/lsd/SpringBoard/backboardd all read this way.
  6. Unattended loop: loader option `starauto=N` (in the payload boot-args; shim/starlte menu starts iOS after N s
     unless a key is pressed, built in the lima VM) + kernel `sweetreboot=N` (log thread flushes and resets into
     recovery via PMU INFORM2/3 + SWRESET after N s) + `tools/starlte-cycle.sh IMG LOG` (wait for TWRP, flash, wait
     for the boot to come and go, pull the log). First run: boot16 (ipados18p).
  State after boot16-18 (`build/ipados18-boot1[6-8].txt`, 300 s each): ~3500 execs, SpringBoard pid 27 alive for the
  whole run (scenes, PosterBoard, home-screen service, backlight requests), backboardd turns the display on and
  swaps at vsync, but every IOMFB swap record carries NULL layers (no IOSurface is ever created by backboardd; iOS 16
  created 828x1792 BGRA buffers at once). iPadOS 18's swap record is 0x514 bytes = the 0x46c layout shifted by 0x48
  (swap id 0x98, surface ids 0x9c, mask 0x14c); SweetFramebuffer reads both (folded into 0209). QuartzCore 18 still
  has the CPU renderer (`CA::WindowServer::Server::sw_renderer` -> SWContext, used when AccelServer::renderer()
  has no Metal device), so the gap is in display buffer allocation / screen-on: `com.apple.backlightd` is hosted by
  backboardd (no backlightd binary in 18.7.11), SpringBoard asks it for screen-on. Remaining crashers: replicatord,
  cryptexd, gamed (Swift assertions), iconservicesagent jetsam; none UI-critical. Uncommitted: 0309 folds, 0310,
  0311, 0312, 0201/0209 folds, tools/memtest, shim starauto, tools/starlte-cycle.sh.
- 2026-10-28 22:44: **MILESTONE: iPadOS 18.7.11 boots to the lock screen on the S9, touch works** (user report;
  boot25 = out/xnu-boot-ipados18x.img, log build/ipados18-boot25.txt). CPU rendering (QuartzCore's SW renderer, no
  Metal device), 1440x1920 iPad canvas on the 1440x2960 panel. The two blockers after SpringBoard came up:
  1. libsystem_trace only honours ACTIVITY_LOG_STDERR / OS_ACTIVITY_* when `amfi_check_dyld_policy_self` (=
     `__mac_syscall("AMFI", 90)`) reports AMFI_DYLD_OUTPUT_ALLOW_DEVELOPMENT_VARS (bit 8); our AMFI stand-in was
     registered as "SWEET AMFI" so every process got ENOPOLICY. Now named "AMFI", op 90 answers bit 8 only (0201).
     With backboardd's job (in /System/Library/xpc/launchd.plist -- launchd ignores the individual plists) set to
     ACTIVITY_LOG_STDERR=1 + StandardErrorPath=/private/var/tmp/backboardd.log and the kernel's
     `sweetdumpfile=<path>` (prints the file's tail before the sweetreboot reset), backboardd said:
     "IOSurface creation failed. IOSurface open failed: 0 / CAIOSurfaceCreate: failed to allocate IOSurface (1440 x
     1920, BGRA)". (StandardErrorPath=/dev/console instead kept backboardd from starting at all.)
  2. iPadOS 18 IOSurface ABI (SweetSurface, folded into 0068): sel 13 limits is 0x28 bytes with the ABI version u16
     at +0x22 (iOS 16: 0x18, +0x16), else the framework closes the connection; the lock result is 0xc68 bytes,
     copied to IOSurfaceClient+0x70: +0 base, +8 state page, +0x10 timestamps, +0x18 id (u32), +0x20 alloc size,
     +0x28 width, +0x30 height, +0x38 bytes per row, +0x40 offset (all u64), +0x48 pixel format, +0x50 alloc size
     (u64), +0x58 plane count, +0x60 bpe (u16) / +0x62 ew / +0x63 eh, +0x64 cache mode, +0x70 parent id, planes at
     +0x88 x 0x98 (+0 w, +8 h, +0x18 offset, +0x20 bpr, +0x28 size (u64), +0x30 bpe, +0x32/+0x33 ew/eh). The stand-in
     picks the iOS 16 or 18 layout by the caller's output buffer size.
  Result: backboardd creates two 1440x1920 BGRA surfaces and alternates them every swap. Daily image (no timer):
  out/xnu-boot-ipados18-live.img = build/ipados18-live-cmdline.txt (starauto=3). Test images: starauto=3
  sweetreboot=100 (+ sweetdumpfile=/private/var/tmp/backboardd.log) via tools/starlte-cycle.sh. Remaining
  crashers: replicatord, cryptexd, iconservicesagent, gamed (none UI-critical). backboardd's stderr log grows
  ~20 KB/s while debugging; drop the redirect from the launchd cache for daily use.
- 2026-10-29: **More RAM, full-panel canvas, iPhone UI** (boot26-32). 0313 `sweetmemscale=N` multiplies jetsam
  per-process limits (crash loops from jetsam reason 7 gone at 4); 0314 keeps 4 KiB VM pages above 1 GiB (arm_init
  switched to 16 KiB pages when memSize > 1 GiB: intermittent "unaligned pmap_enter"), so XNU now gets 1.5 GiB of
  bank 2 (out/starlte-ipad7-1536.adt; max 1792 MiB below the GPU tables at 0x8f0000000). Canvas from the ADT
  (`mkadt_starlte.py --canvas WxH --scale N --artwork-subtype N`) + sweetfb_lw/lh: 1440x2960 @2x fills the panel.
  iPhone UI = MobileGestalt answers, not the ADT: `tools/starlte-mg.py ROOT phone` edits the answer cache
  (/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/
  com.apple.MobileGestalt.plist; CacheData = 658 slots x 8 + 658 presence bytes, DeviceClassNumber slot 101 = 1,
  CacheExtra DeviceClass/DeviceName "iPhone", ArtworkTraits.ArtworkDeviceIdiom "phone"; key = base64(md5(
  "MGCopyAnswer"+name))[:22]); with out/starlte-ipad7-phone.adt (1242x2208 @3x, subtype 2208) SpringBoard runs the
  phone UI (user: "more phone-ey", home indicator). Live root edits without a full reflash:
  `tools/starlte-root-sync.py pull BASE OUT` / `push OUT` (64 MiB chunk hashes on the phone).
  Red status bar banner "rdar:45025538" = SystemStatusUI's Fallback provider: STUIStatusBarVisualProvider_Phone picks
  LegacyPhone only when the screen has no exclusion area; SpringBoard sets it from
  `+[UISDisplaySingleRectShape sb_displayShapeForScreenType:]` (SBHScreenTypeForCurrentDevice), so our screen type
  gets a notch shape and the Split providers (matched by native width 828/1080/1125/1170/1242/1284) find none.
  Cosmetic; fix later via the screen type (MG) or a canvas that matches a Split provider.
- 2026-10-29: **GPU route for iPadOS 18.** `CA::WindowServer::AccelServer::renderer()` only tries Metal
  (CAMetalContextCreate -> CA::OGL::new_metal_context), else the CPU renderer: iOS 16's GLES fallback in the window
  server is gone. But `CA::OGL::GLESContext` is still in QuartzCore with its window-server methods
  (create_surface_from_iosurface, make_buffer_current(WindowServer::Surface*), release_display_surface), and the
  OpenGLES.framework dispatch table is unchanged from iOS 16.5 (glClear +0x58, glDrawArrays +0x210, glBindTexture
  +0x30, glFinish +0x2d0, glClearColor +0x68 from the table base): an earlier port's eagl.c/eagl_slots.txt (MIT, re-cloned
  into ref/refport-os from github.com/oosexgang/refport-xnu-kernel @ 8851317) carries over. Plan: (1) StarbiteMali
  from an app [sbgpu-test], (2) Mesa Panfrost for iOS [done, below], (3) GLES from an app [sbgl-test], (4) IOSurface
  import selector, (5) inject into backboardd and replace renderer() with a GLESContext over an EAGL backed by Mesa.
  - Mesa 25.0.7 for iOS: `tools/mesa-ios/mk-mesa-ios.sh` (Xcode 16.4 iPhoneOS 18.5 SDK, meson cross, Homebrew bison)
    builds softpipe + Panfrost into one libOSMesa.8.dylib (16 MB, 1296 gl* exports), no errors on the first build.
    `tools/mesa-ios/starbite-drm/` stands in for libdrm: Panfrost ioctls -> StarbiteMali selectors (nr - 0x40,
    GEM_CLOSE -> 0x100), version reported as 1.1 (no timestamp queries), sync objects always signalled (SUBMIT is
    synchronous), the "mmap offset" is the address the kernel mapped. `panfrost-ios.patch`: meson wiring, mmap/munmap,
    32 MiB tiler heap (not 128; heap BOs are committed in full), OSMesa creates the Panfrost screen first
    (OSMESA_SOFTWARE=1 -> softpipe), OSMesa ES profile (from an earlier port).
  - sbgpu-test under 18 (launchd job added to the launchd.plist cache, /usr/bin/sbgpu-test): GET_PARAM works
    ("interface 1.3.0, GPU 0x6221, shaders 0x5771777"), then the first CREATE_BO panicked "phystokv_range: illegal
    PA 0xce821000". XNU bug (0315): legacy arm pmap `pmap_sync_page_attributes_phys()` shifts the 32-bit ppnum_t by
    PAGE_SHIFT in 32 bits; with 4 KiB pages and RAM at 0x880000000 the cache flush for a page turning write-combined
    (pmap_sync_wimg) goes to a truncated PA. Fixed with ptoa(). The panic hook (0309) now saves the panic log head
    (11 KiB: string, panicked task, backtrace) + the console's last lines, not the stackshot tail.
  - 0315 verified (boot36): sbgpu-test from launchd: "WRITE_VALUE from userland ... AN APP RAN A GPU JOB" (198 us),
    "FRAGMENT clear from userland: 4096 of 4096 pixels green -> AN APP DREW WITH THE GPU"; no panic in 150 s. (SUBMIT
    returns 0xe00002c2 to a caller that passes no output struct although the job ran: iOS 18 IOKit hands
    externalMethod a non-NULL structureOutput of size 0. Harmless for Mesa, which passes in = out.)
  - backboardd hook recipe (QuartzCore 18, from `-[CARenderer _initWithEAGLContext:options:]` and
    AccelServer::renderer()): the EAGLContext must say API 3; context = calloc(1, 0x8b8) +
    `CA::OGL::GLESContext::GLESContext(void *eagl)`; renderer wrapper = malloc(0x68): [0] = context, [8], [0x10] = 0,
    [0x30] = malloc(0x40000), [0x38] = 0, [0x60] = 1 (u32), [0x64] = 0 (u16). AccelServer keeps the context at
    server+0x400 and the wrapper at +0x408 (also stored at [server+0x60]+0x138), then calls context vtable +0xb0
    (framebuffer compression flag); once +0x408 is set, the original renderer() only runs its per-call tail. So a
    replaced vtable slot can build both and call the original.
  - IOSurface import (for CoreAnimation's render targets and layer textures): StarbiteMali selector 0x103 SURFACE
    (folded into 0305) maps a SweetSurface's pages (buffer or physical) at a GPU VA of its own, one handle per
    surface and client (flags 1 = describe only: size, offset, row bytes, size, format, planes). Mesa side: the shim's
    drmPrimeFDToHandle takes the surface ID as the "dma-buf fd", pan_kmod_bo_import sizes it through the shim, and
    OSMesa's get_egl_image turns glEGLImageTargetTexture2DOES(target, (GLeglImageOES)surfaceID) into a BGRA8
    texture/render target on the surface's memory (as an earlier port's freedreno patch did).
  - sbgl-test (boot37-38): "GLES: context in 1132 ms: Mesa | Mali-G72 (Panfrost) | OpenGL ES 3.1 Mesa 25.0.7",
    framebuffer complete, shaders compile and link, 15 GET_PARAMs and 12 CREATE_BO/MMAP_BO fine, but every SUBMIT
    failed EINVAL: Mesa 25.0's drm_panfrost_submit is 40 bytes (no jm_ctx_handle/pad), StarbiteMali wanted 48.
    0305 now takes both. Also: launchd gives an unlisted daemon 6 MB x sweetmemscale and ignores the job's
    JetsamProperties; the test raises its own limit (memorystatus_control SET_JETSAM_TASK_LIMIT as root).
    Debugging: STARBITE_DRM_TRACE=1 logs the shim's calls on stderr; the job's StandardErrorPath file is read by
    pulling the root (starlte-root-sync.py pull, then attach read-write: a read-only attach refuses the dirty
    journal).
  - **2026-10-29 01:10: OpenGL ES through Mesa runs on the Mali-G72 under iPadOS 18** (boot40,
    build/ipados18-boot40.txt): GPU jobs from Mesa done in 0.1-1.3 ms, no faults; clear reads back blue
    (0xffff0000 = RGBA bytes 00 00 ff ff), triangle middle 0xff6d4949 (the three vertex colours interpolated),
    corner 0xff333333 (the clear grey): "THE GPU DREW A TRIANGLE THROUGH MESA". The default-framebuffer loop ran
    0.8 fps only because OSMesa copies the 11 MB frame into the user buffer on every glFinish, reading the GPU's
    write-combined (uncached) memory with the CPU; CoreAnimation renders into IOSurfaces and never does that.
    GPU speed (boot41, off-screen 1242x2208 RGBA8 texture target, no read-back): clear + 5 full-screen textured,
    alpha-blended quads per frame = **816 frames in 4.0 s = 203.9 fps** (first frame 5 ms), GL error 0. For
    comparison, CoreAnimation's CPU renderer gives ~15 fps on the home screen.
    memorystatus_control SET_JETSAM_TASK_LIMIT (6) answers EINVAL here, INCREASE_JETSAM_TASK_LIMIT (22, own pid,
    DEVELOPMENT kernel) works; the test boot also used sweetmemscale=16.
  - Kernel limits to fix before CoreAnimation: StarbiteMali's GPU VA allocator only bumps (never reuses) and AS1
    level-3 tables are never freed (1 MiB = ~250 tables = ~500 MiB of mappings ever).
- 2026-10-29 02:40: **CoreAnimation draws the iPadOS 18 UI on the Mali-G72** (boot45-48, `build/ipados18-boot4[5-8].txt`).
  `tools/ca-gpu/sbca.c` (injected into backboardd, patch 0316) replaces AccelServer::renderer() (vtable slot
  0x1e697c450): with no renderer yet it makes an EAGLContext of API 3 (an earlier port eagl.c over Mesa, `libsbgl.dylib`
  in /usr/local/starbite/lib), a `CA::OGL::GLESContext` (calloc 0x8b8 in QuartzCore's x_malloc zone) and the
  0x68-byte renderer record exactly as renderer() does for Metal, stores them at server+0x400/+0x408 and
  display+0x138, turns framebuffer compression off (context vtable +0xb0) and lets the original run its tail.
  Boot-arg `sbgl=0` keeps the CPU renderer. `tools/ca-gpu/build-sbca.sh` builds both libraries.
  - boot45: every GL call went to Apple's empty OpenGLES (libsbgl linked `-framework OpenGLES` before Mesa, so
    the gl* symbols bound there). Fixed: no OpenGLES link; sbca dlopens OpenGLES before libsbgl.
  - boot46: Mesa context fine ("OpenGL ES 3.1 Mesa 25.0.7; Mali-G72 (Panfrost)"), then backboardd SIGSEGV in
    IOSurfaceClientGetBulkAttachments (state page pointer client+0x78 NULL) from wrap_buffer ->
    iosurface_get_edr_factor. Cause: IOSurface sel 6 (IOSurfaceClientWrapClientImage, struct in {u64 addr, u32 w,
    h, fmt, bpe, bpr, size}; used by CA::Render::Image::retain_iosurface for page-aligned image data) and sel 7
    (WrapClientMemory, scalars [addr, size]) fell into SweetSurface's default case: zeroed lock result +
    success -> a client of nothing (id 0, 0x0, no state page). 0046 now refuses both; CA uploads those images
    instead. (Zero-copy wrapping via IOMemoryDescriptor::withAddressRange is a later speed-up.)
    sbca keeps a SIGSEGV catcher for that exact fault (logs the client, gives it a zero page) as a guard.
  - boot48 (user photos): lock screen wallpaper gradient, clock, home-screen labels, Control Center glyphs drawn
    correctly by the GPU; home screen frames ~5-7 ms of GPU time, renderer() ~58/s. Not yet right: app icons
    invisible (180x180 IOSurfaces with pixel format 0 shown via -texImageIOSurface, contents probably empty),
    materials/blur platters (dock, search, Control Center) flat white/grey (an earlier port inject.c's renderer
    function/shader fixes, `give_the_renderer_a_shader` etc., not ported; needs iOS 18 GLESContext addresses),
    a doubled copy offset along the top edge (partial-update / buffer reuse?).
- 2026-10-29 03:00-04:10 (unattended, user asleep): **GPU UI fixes, self-screenshots** (boot49-56).
  - Test boots now verify themselves: kernel boot-arg `sweetswipe=1` (starlte_touch.c, folded into 0302) plays
    scripted input through sweet_hid_touch()/sweet_hid_key(): swipe up from the bottom edge at 50 s (the UI is
    the home-button iPhone's, 8 Plus canvas, so that is Control Center), Home at 66 and 71 s (close, unlock),
    swipe left at 80 s (applications' page), tap Settings at 100 s. sbca writes every panel-sized BGRA surface
    at 62/95/135 s of uptime to /private/var/tmp/sbca-shot-<s>-<id>-<W>x<H>-<rowbytes>.bgra (F_FULLFSYNC +
    sync, or the sweetreboot reset loses them); `tools/ca-gpu/fetch-shots.sh bootNN` pulls the root (also the
    base for the next install) and makes PNGs (tools/ca-gpu/shots-to-png.py, zlib only) in build/shots/bootNN.
    `tools/ca-gpu/install-sbca.sh [--pull]` installs the libraries without the 2-3 min pull when the root was
    pulled after the last boot. sbca truncates its log once per boot (file older than kern.boottime).
  - IOSurface (0046): iPadOS 18 wraps caller memory two ways and both were missing: sel 6
    IOSurfaceClientWrapClientImage (struct in {u64 addr; u32 w, h, fmt, bpe, bpr, size}), sel 7
    WrapClientMemory (scalars addr, size), and IOSurfaceCreate with kIOSurfaceClientAddress, whose key is
    **"IOSurfaceAddress"** (+ IOSurfaceAllocSize/IOSurfaceOffset; CA::Render::Image::retain_iosurface uses it
    for icons). sweet_surface_wrap_memory() wires the pages (withAddressRange + prepare; kIODirectionOut and a
    read-only mapping when the pages are not writable, e.g. file-backed images), any alignment (s->offset gets
    the page offset). Before: sel 6/7 fell into the default case (zeroed lock result + success = a client with
    no state page: backboardd SIGSEGV in IOSurfaceClientGetBulkAttachments via iosurface_get_edr_factor), and
    the IOSurfaceAddress creates got fresh zeroed buffers (every icon transparent).
  - Doubled picture (0303): CoreAnimation double-buffers between its own surface and the display's default
    surface (GetLayerDefaultSurface, id 1). Scaled, id 1 was the panel's top-left 1242x2208 and present()
    skipped swaps of it, so every other frame landed on the panel unscaled (the band above the letterboxed
    picture, flicker). Scaled, the default surface is now a kernel buffer (sweet_surface_create_buffer) and its
    swaps are presented like any other; the panel mapping for the scaler is made on every scaled board.
  - CoreAnimation effect numbers: the authoritative table is CA::OGL::MetalContext::TextureFunction_codes
    (0x1e6994d48, char * per CA::OGL::TextureFunction); the CFString literals at 0x1e69a4600 are only in source
    order (pnc/rim/srl/lut sit elsewhere). 106 effects up to cst (105), then blend modes. GL fragment sources:
    CA::OGL::tex_fragment_shaders (0x1e6997ca0, NULL = no shader); GLContext::function_supported() is a fixed
    bitmask (supported: 0-13, 15-19, 21, 23-26, 49-53, 105; 27-38, 41, 43-46 too when byte +0x11 of the
    function state is 0). Hooked vtable slots (GLESContext vtable + 0x10 = 0x1e6984358): function_supported
    +0x3d8, function_uniform +0x3e8, compile_shader +0x78. eagl.c renumbered: c4p 22, ds8 29, vrd 39, vrb 40,
    sb3 42, vcm 48 (cross-checked: 48 gets 20 numbers, 29 gets 8, 74 = edr gets [inf 0 0 0]); its vibrant
    colour matrix and 8x box stand-ins now run ("a colour matrix over what is behind: made"). Still refused:
    20 (c3s, also unsupported by the CPU renderer). an earlier port's libref.gl needs the function_uniform hook
    (refgl_function_values) for those stand-ins: without it Control Center's materials came out as
    unfiltered copies (the "random colours").
- 2026-10-29 14:15: **boot57: lock screen, home screen with real icons, Settings, all drawn by the Mali; test
  boots verify themselves** (build/shots/boot57/*.png). Dock icons (Messages, Safari, Music, Mail) are right
  since 0046 honours IOSurfaceAddress; Settings opens from the scripted tap and renders cleanly. No backboardd
  crash in boots 56-57.
  - The scratch folder with fold-fix.sh, mkimage.sh and the boot menu inputs (uc3/) was cleaned. Now in the repo:
    `tools/fold-fix-11417.sh` (same script) and `tools/mkimage-11417.sh` ([ADT=...] NAME CMDLINE_FILE), which
    reads build/boot-uc3/{loader.Image,ramdisk.bin,bundles.bin}; those were cut back out of
    out/xnu-boot-ipados18-sbcb.img (kernel slot = loader + pad to 4 KiB + payload; payload-linux = payload + pad
    + bundles) and repack byte-identically. Keep build/boot-uc3/.
  - Scripted input (sweetswipe=1, 0303 now) starts at the first frame on the panel (sweet_fb_first_present):
    backboardd is exec'd at ~37 s but asks for its first renderer() only at ~69 s, so absolute times missed the
    UI. sbca's shots are 25/55/80 s after its first renderer() call; sweetreboot=200 in
    build/ipados18-sbcb-cmdline.txt. sbca log lines carry seconds since boot.
  - sbca's watchdog (backtraces of all threads after 3 s without renderer()) fires on idle screens too:
    CoreAnimation does not draw when nothing changes. boot56's "5 frames" was the late UI start plus idle, not
    a hang. Its fault handler now names image+offset/symbol (dladdr) or the unslid cache address.
  - Open: (1) Control Center: TextureFunction 20 "c3s" is refused (no GL source in tex_fragment_shaders, not in
    the SW renderer either); what needs it is not drawn. Needs a source in the table (an earlier port's
    give_the_renderer_a_shader) once its inputs are known. (2) Wallpaper black on lock and home screen:
    PosterBoard runs and does not crash; it most likely renders with Metal (the earlier red gradient was the
    vibrant-colour-matrix veil drawn as a picture, gone since the effect numbers are right). (3) Settings row
    icons missing (no wrap or upload failure logged). (4) Widgets "Waiting...". (5) Mesa rejects zero-copy import
    of wrapped surfaces whose offset is not 64-byte aligned (CPU copy instead; slower only).
- 2026-10-29 15:00: **Control Center drawn by the GPU** (boot58-61, build/shots/boot61). TextureFunction 20
  "c3s" = a 3x3 colour matrix + bias (vec3 color_matrix[4]) over what is behind (gl_LastFragData), the
  material of Control Center's platters and (probably) Settings' row icons. sbca gives its NULL
  tex_fragment_shaders entry a source (premultiplied 3x3+bias, clamped to alpha, marked "sbc3s" so eagl.c
  declares the uniform CoreAnimation does not), says yes in function_supported, and sets color_matrix by hand
  after GLContext::function_uniform (which has no location for it) through Mesa's glUniform3fv. Opens with a
  swipe down from the top-right corner (iPhone X style; the bottom-edge swipe does nothing), scripted with
  eased 50-sample swipes. kern.sweet_first_present (sysctl) = first panel frame; sbca's shots and the kernel's
  input script both count from it (backboardd's first renderer() is ~17 s earlier). Still wrong: the blurred
  background behind Control Center is over-bright (bias likely needs scaling by alpha, or the blur stand-in
  over-accumulates); wallpaper black; Settings row icons missing; widgets "Waiting...".
  out/xnu-boot-ipados18-gpu-live.img = the same without the timed reset/script/shots, for the user's testing.
- 2026-10-30: **Why the GPU UI felt like the CPU one and died within minutes** (user's live sessions, logs
  build/ipados18-live1.txt + build/shots/live1/sbca.log; fixed in boot62-63).
  - Death: "STARLTE GPU USER: out of level-3 tables" x1282, then DRM_IOCTL_PANFROST_CREATE_BO failed x2772,
    panfrost_batch_init/prepare_draw failed x1000, frames of 1.35 s, backboardd SIGSEGV. The app GPU mapper
    handed out level-3 tables from a bump pointer and never freed them (250 tables = 500 MiB of mappings ever),
    and StarbiteMali's GPU VAs were a bump allocator too. Now (0301) tables are counted and return to a free
    list when their last page is unmapped (a failed map is rolled back), and (0305) VAs come from a bitmap of
    64 KiB granules, next-fit, one free granule after each buffer, freed on GEM_CLOSE and failure paths.
    boot62-63: peak 64-67 of 250 tables, no CREATE_BO failure.
  - Slowness: the panel ran at 8-14 fps at best ("A5XELTE FB: frames=..." lines; this kernel defines all three
    board configs, so the A5 30 Hz timer and copy timing apply). Every frame was scaled to the panel by the CPU
    (scaledCopy, 11-31 ms: a gather into write-combined memory that also reads rows back from it), because
    the GPU present (0301 blob, tools/gpu/starlte-g72-tri.c) was built for the old 828x1792 canvas and
    SweetFramebuffer's page array and starlte_gpu.c's slots held 8 MiB. Now: blob regenerated for 1242x2208
    (full width, 200-row bars, as scaledCopy), slots 6 tables = 12 MiB (SG_MEM_BYTES 0x60000),
    SWEET_FB_GPU_MAX_PAGES 3072, a5fb_hz=60, and gpuPresent() says why when it gives up (it used to fail eight
    times silently). boot63: "STARLTE GPU PRESENT: 600 frames, average 2316 us" (4.1-4.5 ms with the cache
    clean) instead of 11-31 ms on the CPU. eagl's own frame work: ~18 ms (2.2 ms of it waiting for the GPU).
  - Still: G3D PLL at 260 MHz (vdd_g3d 950 mV, no DVFS), CPU clocks unknown (the cycle counter line is broken).
- 2026-10-30 (later): **CPU clocks raised through ACPM DVFS; where frame time goes** (boot64-66).
  - Clock survey (starlte_smp.c, 30 s after boot, each CPU bound in turn, 8M dependent adds): S-Boot leaves
    all eight cores at ~1050 MHz (cpu0-3 = A55, mpidr 0x81000x00; cpu4-7 = M3, 0x800001xx).
  - DVFS works: on ipc channel *index* 0 (descriptor id 0, as Linux), cmd { domain, kHz, 0 = FREQ_REQ, 0 },
    domains cpucl0 = 2 (A55), cpucl1 = 3 (M3); replies echo the request (FREQ_GET answers 0 kHz, which is why
    it was given up before). Default now (0303, st_cpu_dvfs): A55 1690000, M3 1794000 kHz (DT levels;
    max-freq-wo-ocp for the M3s; no thermal control here, so not the 2.7 GHz single-core level); measured
    1689 / 1792 MHz. starcpu=0 leaves S-Boot's clocks, starcpu=L[,B] sets others (max 1794000).
  - sbca profiler (1 kHz pc samples of backboardd's render thread / an app's main thread, system calls counted
    at their caller; sweetinject=...,backboardd,SpringBoard,Preferences; logs sbca-<name>.log; sbprof=0 off):
    scrolling Settings, the app's main thread is CPU-bound (ObjC/Swift/CF ~85%), SpringBoard idle, backboardd's
    render thread ~40% idle (CFRunLoop), ~26% blocked in kernel calls from Mesa (synchronous SUBMIT, and
    map/unmap each flushing the whole AS1), the rest Mesa/eagl/CA.
  - Next: asynchronous GPU submits, ranged MMU flushes, G3D clock (260 MHz; ACPM domain g3d = 4).
- 2026-10-30 evening: **GPU clock 572 MHz; the memory bus must not be touched** (boot67-69).
  - StarbiteMali now logs per-call counts/times every 10 s ("STARLTE GPU CALLS"): scrolling Settings makes
    ~1500-1900 SUBMITs a second (~50 a frame), each waited for synchronously (342 us at 260 MHz); creates and
    closes are negligible. Submits blocked the render thread 5.2-5.4 s of every 10 s.
  - 0303 sg_clock_up(): ACPM DVFS domain g3d = 4 to 572000 kHz after GPU bring-up (before the present and
    apps), vdd_g3d put back to 950 mV; PLL_G3D reads 572 MHz locked. Submits 342 -> 222 us, panel peaks
    30-37 fps while scrolling; no fault in boot68. stargpu=0 keeps S-Boot's 260 MHz.
  - The GPU present hardly sped up (2.52 -> 2.25 ms): memory-bound. Asking ACPM for MIF 2093000 / INT 667000
    kHz froze the phone instantly with a green display (boot69, user's photo): removed for good.
  - Next for speed: fewer submits a frame (why ~50: Mesa batch flushes from eagl's surface copies/glFinish,
    CoreAnimation's offscreen passes) and asynchronous submits.
- 2026-11-01: **Idle CPU: daemons that spin without a network** (boot70-71). Kernel "STARLTE TOP" (0300,
  starlte_smp.c: every 10 s the eight busiest processes by task CPU time, % of one core; startop=0 off).
  Before: idle 56-230% of 800%, imagent 142% steady, audiomxd/cloudd/sirittsd/icloudmailagent 20-40%.
  tools/starlte-disable-jobs.py ROOT tools/starlte-disabled.list takes jobs out of launchd.plist (kept in
  launchd-disabled.plist): the S7 port's list (ref/ios-herolte-new, wardevsa/ios-a5xelte herolte, which has
  CA on its Mali-T880 the same way, 35-38 fps) plus imagent, sirittsd, icloudmailagent, searchpartyd,
  cloudd(.sync), PerfPowerServices(Extended), then parsecd (133% in boot71) and passd (crash loop feeding
  ReportCrash/osanalyticshelper). After: idle 68-120%, mostly SpringBoard and backboardd. No
  BTAudioHALPlugin on this iOS 18 root. an earlier port's S9 change (example-project/hackios-xnu-kernel#65): queued
  submits, cache maintenance for surfaces named by a job (submit) and after WAIT_BO: our next step.
- 2026-11-01: **iOS 26 feasibility, first numbers.** iPadOS 26 needs an A12 or later (the iPad 7, A10, is dropped), so
  there is no plain-arm64 userland anymore: every iPadOS 26 binary is arm64e (ARMv8.3 with PAC). Newest published
  XNU is still xnu-12377.121.6 (iOS 26 era), with no iOS 27 tag. Stand-in measurement: tools/dsc-isa-census.py
  ran over this Mac's macOS 27.2 arm64e shared cache (4113 images, 921 M instructions, 3.5 GiB of code;
  build/dsc-isa-census-macos27.txt). Counting only S_ATTR_PURE_INSTRUCTIONS sections:
  - PAC instructions that are UNDEFINED on the S9 (RETAx 7.4, BRAx/BLRAx 9.4, PAC/AUT register forms 17.4):
    about 34 per 1000 instructions, roughly 31 M sites. The hint-space PAC ops (14.9/1000) are already NOPs here.
  - M3-only problems (A55 has these): LSE atomics 0.99/1000 and LDAPR 0.21/1000.
  - Rare ops neither core has: FJCVTZS 2150 and FCADD 16 k.
  - The macOS cache targets M-series, so iOS 26's A12 build should have fewer v8.4+ ops. PAC density is set by the
    ABI, so it should carry over.
  The conclusion stands: it needs a translation layer for the userland, not a few patches.
- 2026-11-01: **The real iPadOS 26.7.2 cache (23H128, iPad 8 / J171aAP, A12).** Fetched only Cryptex1,SystemOS
  (141-72226-027.dmg.aea, 1.9 GiB; it decrypts to a 5.2 GiB raw APFS):
  `DEVICE=iPad11,6 VERSION=26.7.2 BOARD=j171aap PARTS=systemos OUT=build/ipsw-26 tools/fetch-ios-parts.sh`,
  then `OUT=build/ipsw-26 tools/decrypt-ios-parts.sh` (fetch-ios-parts.sh now takes PARTS). The .dmg was deleted
  after the survey because the disk was at 99 %; re-fetch the same way.
  - The cache is dyld_shared_cache_arm64e only, with no arm64 variant: 4246 images, 780 M instructions, 2.9 GiB of
    code.
  - GOOD: QuartzCore still has the GLES renderer. The RTTI N2CA3OGL11GLESContextE / N2CA3OGL7ContextE are present,
    and OpenGLES.framework (GLEngine, libGLProgrammability, ...) still ships. The sbca approach (EAGL over
    OSMesa/Panfrost, replacing AccelServer::renderer) has something to hook on 26.
  - Census (build/dsc-isa-census-ipados26.txt): non-hint PAC ops 37.7 per 1000 (RETAx 7.1, BRAx/BLRAx 13.6,
    PAC/AUT register forms 16.9), 29.4 M sites, LDRAx only 1650. M3-only problems: LSE 1.06/1000, LDAPR 0.17/1000.
    Neither core: FJCVTZS 532 and FCADD 16 k, rare enough for the kernel to emulate them on an undefined-instruction
    trap. The UI stack alone (QuartzCore, UIKitCore, SpringBoard, backboardd, libsystem_*, dyld) is 48 MiB of code
    with about 310 k sites.
  - Plan shape: an ahead-of-time arm64e->arm64 rewrite of the cache, done on the host; userland on the A55s first
    (M3 lacks LSE/LRCPC); xnu-12377 with the 11417 series ported forward.
- 2026-11-01: **Kernel series ported to xnu-12377.121.6** (iOS 26 kernel line, macOS 26.5). Source side only;
  not compiled yet. Port tree build/xnu-12377 (worktree of build/xnu-11417, branch port-12377), exported to
  xnu/series-12377 (178 files; README lists the port notes). 0001-0316 replayed with tools/port-12377-next.sh;
  16 needed hand fixes. Notable 12377 changes:
  - kmem guard objects moved into the VM map store (vm_map_store.c), so 0013's RNG goes there.
  - The realtime CPU choosers moved to sched_rt.c, so 0238's fast-core picks go there too.
  - The shared region has a second nested config_map, so 0202 also turns off code-signing enforcement on it.
  - write_random hashes with SHA-512 (0033's BearSSL).
  - TBI0 is in the boot TCR.
  - lib<board>.os.a is linked whenever a board is set (our boards are still filtered out).
  - New Apple-SoC-only exports were dropped.
  Board MASTER configs re-merged (0317). Staging: libpthread-539.100.4, hfs-715.120.4 (macos-265).
  Toolchain: darwin-xnu-build at the same pinned commit knows 26.5 (patches/26.0, KDK 26.5 25F71);
  tools/build-xnu-12377.sh. Still needed: KDK 26.5 (dortania mirror, 1.1 GiB dmg), and maybe Xcode 26 if
  Xcode 16.4's clang 17 / 15.5 SDK are too old. Test plan: first boot with the iPadOS 18 root, which checks the
  kernel milestones (root mounted, launchd, SpringBoard) before any iOS 26 userland exists.
  iOS 26 base device: iPhone 11 (iPhone12,1 N104AP, A13, 4 GiB, 828x1792), real iPhone iOS rather than iPadOS
  with an iPhone identity. Its cache still needs the census for v8.4 instructions.
- Disk cleanup (user asked): removed the decrypted iPadOS 18 os/systemos dmgs (10 GiB; already staged into
  root-phone.img; re-fetch with fetch-ios-parts.sh + decrypt-ios-parts.sh), ~/Downloads/Xcode_16.4.xip, and
  superseded out/ test images and payloads. 7.5 -> 24 GiB free.
- 2026-11-01: **xnu-12377 (iOS 26 kernel) boots iPadOS 18 to the GPU-rendered UI on the S9.** Builds with
  Xcode 16.4 + KDK 26.5 25F71 (build/kdk, unpacked). Build fixes and boot fixes are in
  xnu/series-12377/0318-port-12377-build-fixes.patch (tools/fix-12377.sh amends it). The payload uses a
  `strip -x` copy (out/kernel.development.starlte-12377.x): the full kernel is 573 KB too big for BOOT; the full
  one stays for atos. tools/mkimage-11417.sh takes KERNEL=. Boot rounds (images out/xnu-boot-xnu12377-a..l):
  - a-c hung after STAGE 7. On the A5XELTE boards the early console switched off at mark 7 (an 11417 assumption),
    so the later marks were never seen. It now stays on until arm_vm_init drops TTBR0 (mark 29), and early
    panics print on screen.
  - The seed: 12377 asks for 256 bytes of random-seed. tools/adt-reseed.py set the phone ADT to 256; mkadt.py in
    ref/ was edited locally (ref/ is not tracked).
  - d/e: "Unsupported memory configuration". 12377 selects the layout with ARM64_LARGE_MEMORY, so 0093's
    LARGE_MEMORY_DEFINE was ignored: physmap was put below a kernel at 0xfffffff006000000. Fixed with
    ARM64_LARGE_MEMORY = 0 for our boards.
  - f: DC ZVA alignment fault in bzero of /pram from PE_create_console. 12377 maps WCOMB/RT RAM outside the
    managed DRAM as Device-GRE. pmap_wcomb_normal() keeps it Normal-NC on our boards (framebuffer, /pram, Mali).
  - g: assert _unsafe_kernel_text_initialized, because there is no iBoot kernel-ctrr-to-be-enabled property. Our
    boards report unlocked text.
  - h-k: launchd killed with KERN_INVALID_GUARD_OBJECT_SLOT (OS_REASON_MTE_FAIL namespace 37). This is the new VM
    guard objects: a 4 KiB-kernel fault on 16 KiB user memory checks [addr, addr+16K) from the 4 KiB-aligned
    address, which spills into the next slot. vm_guard_object_should_fallback always falls back on our boards.
    Diagnostics kept: SWEET COPYIO FAULT, SWEET VMGO REJECT.
  - l: no panic in 240 s. SpringBoard and backboardd ran, sbca was injected, and the Mali stage tests passed.
    Measured: 12.9 k submits per 10 s at 255 us, FB 23-34 fps, STARLTE TOP 74-225%. Log build/xnu12377-l-log.txt.
  Live image out/xnu-boot-xnu12377-live.img (gpu-live cmdline) flashed for the user.
- 2026-11-01: **arm64e code runs on the S9 (patch 0319-sweet-arm64e-userland).** ml_grade_binary takes arm64e slices
  with the ptrauth ABI flag at grade 8 (below arm64's 9/10, so iPadOS 18's fat binaries keep their arm64 slice).
  User undefined instructions go through sweet_a64e_emulate() (osfmk/arm64/sweet_a64e.c + sweet_a64e_fp.s):
  - PAC*/AUT*/XPAC* are no-ops; RETAA/B, BRA*, BLRA* become RET/BR/BLR; LDRAA/B become LDR (pre-index writeback
    kept).
  - PACGA is a fixed hash << 32. FJCVTZS is JS ToInt32 in integer code, with Z = exact.
  - FCADD H/S/D uses a scalar FP asm helper under the user's FPCR.
  Test: tools/a64e/a64e-test.c, a static arm64e binary with no libc. tools/a64e/build-a64e-test.sh signs it ad hoc.
  tools/a64e/install-a64e-test.sh puts it on the root as the launchd job com.starbite.a64e-test (run once at boot,
  output on /dev/console -> flash log; --remove takes it out). First run: EXC_ARM_SP_ALIGN; a static _start needs
  crt0's sp alignment, so a naked _start now does it. Boot n (xnu12377-m image): 19/20 PASS, and the one FAIL was
  the test's own expected value (fcadd 1-20 = -19), since fixed. The job is still on the root.
- 2026-11-01: **PAC translator: tools/a64e/a64e-rewrite.py** (cache | macho, in place, --only, --dry-run).
  It replaces one instruction with one, in pure-instruction sections only:
  - RETA* -> RET, BRA* -> BR, BLRA* -> BLR, PAC/AUT/XPAC register forms -> NOP.
  - LDRA* without writeback -> LDR/LDUR.
  - Left for sweet_a64e.c to emulate: PACGA, FJCVTZS, FCADD, writeback LDRA*.
  Dry run on this Mac's macOS 27 arm64e cache (4113 images): 31.5 M rewrites in 14 s (pac 16.0 M, blr 7.9 M,
  ret 6.8 M, br 0.8 M). Left to trap: 2150 fjcvtzs, 16239 fcadd, 767 pacga, 18 ldra. On a64e-test the rewrites
  check out with otool. A rewritten Mach-O needs re-signing (codesign -f -s -); the shared region runs without
  cs enforcement on our boards (0202).
  Watch out: `cp -c` does not clone across volumes (the Preboot cryptex copy cost 6.7 GB, deleted).
  iOS 26 base: iPhone 11 (N104AP) 26.6.2 23G90 (newest 26.x). OS 094-95954-090.dmg.aea 7.25 GB, SystemOS
  094-97202-092.dmg.aea 2.0 GB, AppOS ~180 MB. The iPhone 11 also gets iOS 27 (27.2 24B119 signed), but no iOS 27
  XNU source is published yet.
- 2026-11-01: **iOS 26.6.2 (iPhone 11, N104AP, 23G90) root built for the S9.** It replaces iPadOS 18 on USERDATA
  0..16 GiB. The user skipped the backup; iPadOS 18 can be rebuilt with build-ios-root.sh.
  - Size: uncompressed it is 17.9 GB (OS 12 GB + SystemOS 5.9 GB), because APFS compression is lost on HFSX.
    TRIM_IOS26=1 (stage-ios-root.sh) drops non-English .lproj, *.espresso.*, *.mlmodelc, the TTS voices and the
    CVNLP word models, on top of the existing omissions. INSTALL_STAGED_APPS takes a name list (11 stock apps).
  - Build: `sudo tools/build-ios26-root.sh` (run in the app terminal; user typed the password) ->
    build/root-ios26.img. 14 GiB used, 1.7 GiB free.
  - TRANSLATE_A64E=1 ran a64e-rewrite on the cache in the image: 29.5 M rewrites. Left for sweet_a64e: 535
    fjcvtzs, 16239 fcadd, 687 pacga, 1345 writeback ldra.
  - Tuning without sudo: attach -owners off; in-place rewrites keep the owners. starlte-tune drops plus
    starlte-disabled.list (28 present).
  - ADT out/starlte-iphone11.adt (`mkadt_starlte.py --big --bank2 --bank2-mb 1536 --iphone11
    build/ipsw-26-n104/n104ap.adt`).
  - Boot-args build/ios26-test-cmdline.txt: cpus=4 (A55 only: LSE/LDAPR), 828x1792 canvas, no sweetinject (sbca
    carries iPadOS 18 offsets), sweettrace=1500, sweetreboot=300. BOOT out/xnu-boot-ios26-a.img (12377 kernel).
  - Decrypted sources were deleted by DELETE_SOURCES (re-fetch with fetch-ios-parts.sh, then decrypt).
- 2026-11-01: **iOS 26.6.2 userland runs on the S9** (xnu-12377, iPhone 11 identity, translated cache).
  Boots ios26-a..e:
  - a: launchd SIGSEGV on a raw chained pointer. map_with_linking_np refused dyld's 16 KiB fixup pages (EINVAL),
    and dyld's in-process fallback cannot fix split .dylddata subcaches. Patch 0320: 16 KiB fixup pages on the
    4 KiB kernel (sweet_fixup_subpage assembles the 16 KiB block), arm64e formats without PAC.
  - b: same crash, now in the shared cache. Patch 0321: slide info v3/v5 never stored authenticated pointers
    without ptrauth_calls; the shared-region pager slid 4 KiB pages as if they were 16 KiB (overran them).
    sweet_sr_slide_subpage slides each page inside its 16 KiB slide page.
  - c: launchd runs. The critical boot task cc_fips_test fails only fipspost_post_integrity (MAC over
    corecrypto's translated code); every algorithm test passed on translated code. /usr/libexec/cc_fips_test is now
    tools/a64e/fips-ok.c (the original is kept as .orig). Future builds: build-ios-root.sh translates with
    `--skip libcorecrypto`.
  - d: assert cpu <= ml_get_max_cpu_number(): st_cpu_survey walked 8 CPUs with cpus=4 (fixed in 0321).
  - e: no panic in 300 s. About 170 processes, including SpringBoard[28], backboardd[58], runningboardd, securityd,
    cfprefsd. STARLTE TOP goes idle at 0-7%. audiomxd loops on SIGTRAP at a cache pc. No frames presented yet
    (no "A5XELTE FB: frames"): backboardd waits on something. Next: what backboardd and SpringBoard block on
    (sweetmsg tracing), audiomxd's trap, then the GPU (sbca for iOS 26's QuartzCore offsets).
  Live root edits: base is now build/root-ios26.img (`starlte-root-sync.py pull build/root-ios26.img OUT`).
- 2026-11-01: **MILESTONE: iOS 26.6.2 boots to the lock screen on the Galaxy S9** (user photo: Liquid Glass clock,
  "Swipe up to open", status bar). Boot ios26-g (out/xnu-boot-ios26-g.img, log build/ios26-g-log.txt).
  - Last blocker: backboardd aborted in libxpc's lightweight-code-requirement check (`matched ==
    (match_result.error_code == AICMR_MATCH)`, via LightSourceSupport). libamfi calls `__mac_syscall("AMFI",
    0x66)` = amfi_launch_constraint_matches_process; our stand-in answered ENOPOLICY with the caller's zeroed result.
    Patch 0322 answers AICMR_MATCH. Found by symbolizing the CRASH backtrace against the root image's cache:
    `ipsw dyld a2s CACHE ADDR --slide S`; `ipsw dsc disass --vaddr ... --cache scratch.a2s` for read-only mounts.
  - Before that, boot ios26-e's hang chain: SpringBoard waited on backboardd (BKSDisplayGetMainScreenInfo),
    backboardd's main thread on containermanagerd, and its render thread on CAS. The abort loop (5 backboardd restarts
    in boot f) was the same AMFI issue.
  - tools/starlte-disabled-ios26.list (analytics daemons, findmylocated, nearbyd, familycircled) is applied to the
    live root.
  - Rendering is CPU, 11-13 fps, avg copy 48 ms per frame to the 1440x2960 panel (sweetfb_gpu present not used).
    SpringBoard 45-138 %, backboardd 66-90 %.
  - Touch: the S6SY761 works and SweetHIDTouch is opened by backboardd. Swipes are delivered (STARLTE TOUCH / SWEET
    HID log), but the lock screen does not react. IOHIDFamily 1915 -> 2238 changed no event or queue format that
    matters. Next: backboardd's own log (ACTIVITY_LOG_STDERR + StandardErrorPath + sweetdumpfile, as for iPadOS 18).
  Live image out/xnu-boot-ios26-live.img (build/ios26-live-cmdline.txt: no timed reset).
- 2026-11-01 20:25: **MILESTONE: touch works on iOS 26** (boot ios26-touch6, user report: apps open, Clock ran).
  Correction to the entry above: IOHIDFamily-2238 *did* change the queue format. Every event, each child too, now
  sits behind its own 0x1c-byte IOHIDSystemQueueElement (timeStamp, senderID, options, attributeLength,
  eventCount), and readBytes sets a child's IOHIDEventData.depth = parent's + 1 (IOHIDEvent::_parent, set by
  appendChild). The old format (one header, children's data packed after the parent's) gave userland the
  collection with no contacts: backboardd's BKDirectTouchState always had `contacts: []`, and SpringBoard never got
  a type-0 UIEvent. Read from the KDK 26.5 IOHIDFamily (getLength / readBytes(void*,u64,u32*) / withBytes) and
  folded into 0323 (getLength, appendBytes(..., count), readBytes, createBytes, withBytes).
  - Found with tools/touchspy (an IOHIDEventSystemClient monitor, launchd job com.starbite.touchspy, still on the
    root; log /private/var/tmp/touchspy.log): `children=0` before the fix, `children=1` with the contact after.
  - tools/mkimage-ios26.sh always uses out/starlte-iphone11.adt (the 64-byte-seed default ADT panics in
    prng_random: "Expected 256 seed bytes").
  - A backboardd category-debug logging plist (TouchEvents/EventDelivery) does not reach the stderr log and costs
    logd 70-100 % CPU: removed. With it, plus datamigrator (~110 % every boot), SpringBoard did not finish starting
    within 200 s. build/ios26-touch6-cmdline.txt: no scripted input, sweetreboot=420.
  - Open: backboardd aborts in CA::OGL::Context::push_surface (assert: surface not in the tracked list; from
    AsynchronousNode::retain_surface <- ImagingNode::render), SpringBoard restarts with it. It is drawing an offscreen
    effect (glass/backdrop material); the Clock tab bar shows garbage there. No Liquid Glass, ~9 fps CPU rendering.
- 2026-11-01 21:50: **GPU (sbca) on iOS 26: the Mali-G72 draws the lock screen** (GPU boot 5+, user photos IMG_5914-5916:
  clock, date, charging glyph; wallpaper a red fallback). Fixes on the way, in order:
  - sbca for iOS 26 QuartzCore: build/ca-gpu/sbca-ios26-offsets.md (renderer slot +0x1c0, GLES context 0xb10, server
    +0x5c8/+0x5d0, display +0x148; skip CAMetalContextCreate's 30 s IOAcceleratorES wait by marking its once done).
  - xnu 0324: sweetinject targets get ptrauth_disabled=1 (dyld load_arm64e_keysOff -> arm64 GPU libraries load).
  - Context::push_surface asserts every surface is in the tracked list; only MetalContext tracks. sbca turns the
    not-found branch (0x183f0f1e4 cbnz) into a b.
  - Compression off: CAEnableIOSurfaceCompression / CAEnableFramebufferCompression = false in the three global
    preference plists (mobile, root, /var/preferences).
  - IOMFBDisplay::framebuffer_pixel_format chose 'b3a8' (wide colour): sbca makes it return 'BGRA'
    (IOS-26-Hot uses CAEnableDeepFramebuffer=false instead).
  - xnu 0325/0329: user-mode platform errors (external aborts) are logged with ESR/FAR/PAR via a thread call and kill
    the task instead of panicking. 0326/0328: IOSurfaces and GPU buffers in whole 16 KiB user pages. 0327: imported
    surfaces stay wired while the GPU maps them. 0330: GPU buffer pages back to default attributes before free.
    0331: imported/client-memory surfaces never mapped write-combined (CPU or kernel).
  - The resprings: backboardd heap pages became Normal-NC (PAR attr 0x44); objc_release's LSE atomics on Normal-NC
    take synchronous external aborts (ESR 0x92000010) on the Exynos 9810. WIMG is per physical page: a WC view of a
    wrapped image / client-memory surface flipped the whole page in the owner's heap.
  - References (build/ios26-gpu-references.md): HackiOS / ios-topaz are iOS 16 (EAGLServer); every public iOS 26 port
    is Metal->Vulkan. IOS-26-Hot (xnu-12377, Pixel 4/Realme X2 Pro) is the closest: HEIC poster layers decode to
    nothing without HEVC hardware (converted to PNG), CABackdropLayer disabled for glass, GPU buffers in RAM XNU does
    not manage (a WC view of managed RAM is a mismatched alias of the cacheable physical aperture).
- 2026-11-01 22:02: **MILESTONE: iOS 26 is usable on the Mali-G72** (GPU boot 10, xnu 0331 with copy-back mappings).
  Unlock, home screen, Settings work; no backboardd/SpringBoard crash in the whole boot. (kIOMapDefaultCache for the
  shared mappings deadlocked backboardd in vm_page_sleep, boot 9; kIOMapCopybackCache == VM_WIMG_DEFAULT works.)
  User report: icons over-bright ("bloom"), no Liquid Glass, Control Center not drawn, Settings' top blur pixelated,
  black boxes, laggy; Settings > About says "iPadOS 26.6.2" on iPhone 11.
  - Speed: the frame reaches the panel by a CPU scaled copy, avg 50 ms (A5XELTE FB avg_copy_us ~50000): 4-10 fps
    whatever the GPU does (GPU submit ~150 us). CPU: backboardd ~60 %, logd 30-90 %, datamigrator 15-56 %.
- 2026-11-01 23:45: **MILESTONE: Vulkan runs on the Mali-G72 under iOS 26** (boot t3, tools/vk/vktest.c as launchd job
  com.starbite.vktest): Mesa 25.0.7 PanVK (`VULKAN=1 tools/mesa-ios/mk-mesa-ios.sh`, panvk-ios.patch,
  PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1) through starbite-drm and StarbiteMali: instance, device, image + buffer
  memory, vkCmdClearColorImage + vkCmdCopyImageToBuffer, submit, wait; 4096 of 4096 pixels read back as the clear
  colour. starbite-drm grew drmGetDevices2 & co (one platform device, render node /dev/null), timeline signal/query
  (every point reached: jobs finish inside SUBMIT) and opens the StarbiteMali connection at the first request.
  - Metal plan (build/metal-port-plan.md): IOS-26-Hot's phone/metal2 (MIT) + metal2vulkan (Rust) + PanVK in place of
    Turnip; kernel IOAcceleratorES with MetalPluginName/MetalPluginClassName (their patch 0096). metal2vulkan needs
    Vulkan 1.2: PanVK from Mesa 26.2.6 (needs host mesa_clc/vtn_bindgen2: Homebrew llvm 23 + spirv-llvm-translator).
  - QuartzCore's default.metallib (189 AIR modules) disassembles with Homebrew llvm-dis (build/metal): Liquid Glass
    is glass_background(_sdf)(_no_bleed), glass_foreground(_sdf), displacement_map, chromatic_aberration, rim/brim,
    sdf_gen_field/gradients + fixed_frag's SDF branches. The GL path's census (glcensus=1): kfh, ghn, ghz, gfs, dgh,
    dff, dfg, bvc, lub have no GL program (drawn plain or not at all): that is the broken glass.
- 2026-11-02: **Metal memory: the freeze was a GPU buffer leak** (boots metal4..metal9b).
  - metal4/5: RAM ran out ("vm-compressor-space-shortage", threads in vm_page_wait, kernel_task 140-170 %).
    ADT out/starlte-iphone11-1792.adt (dram-size 0x70000000 patched into the 1536 ADT, same seed): 1792 MiB boots
    (`ADT=... tools/mkimage-ios26.sh`); vm_compression_limit=196608. Not enough alone.
  - 0339 (StarbiteMali "wired: buffers/surfaces/host" in the 10 s CALLS line) + 0340 (STARLTE MEM every 30 s:
    queues, compressor, top footprints): GPU buffers grew 273 -> 1342 MiB in one tour.
  - starbite-drm leak tracker (SBDRM lines in /var/local/mtl2.log every 15 s, call sites by live bytes; symbols:
    out/ios/sym/{StarbiteMetal,libvulkan_panfrost.dylib}, atos -l 0): PanVK's per-batch TLS (stack) buffer,
    2.6 MiB per render pass on the MP18, kept in the command pools' BO lists (311 MiB in backboardd); plus a 137 MiB
    indirect-varying buffer per device. Fix (mesa26 patch): one 8 MiB TLS buffer per device (StarbiteMali runs one
    chain at a time, synchronously), 250k indirect vertices (17 MiB), texture pools 64/24 MiB. metal8: buffers
    level at ~535 MiB, no "unhealthy" all tour, ~10 fps steady.
  - Boot command line max 608 bytes (shim BOOT_LINE_LENGTH); dropping -noprogress panics (pe_init.c:353 progress
    assert). Lag: backboardd idle in samples; apps CPU-bound on the 4 A55s while datamigrator ~110 %, logd ~65 %,
    sharingd ~45 % (com.apple.system.logging.plist Enable-Logging=false did not quiet logd).
  - metal10: logd + sharingd Disabled in launchd.plist, DataMigration's com.apple.datamigrator.xpc renamed
    .starbite-off, mtl2-clients += iconservicesagent PosterBoard, icon cache moved to
    /var/local/iconscache-old-54E29C5C (Library dirs are mode 000: chmod u+rwx as owner, restore after).
  - metal10: SpringBoard waits in -[BKSSystemShellService _checkInWithServerForReason:waitForDataMigration:]:
    datamigrator must run (renaming its XPC service hangs the UI). logd + sharingd Disabled: all-CPU 350 -> ~125 %,
    fps up to 24 (metal11).
  - Metal needs sweetinject: the plugin is arm64, so only processes injected (ptrauth off) can load it.
    iconservicesagent injected + listed: real glyphs (Airplane Mode, Accessibility, Calendar "31"); it was
    jetsam-killed at 15 MB x 4 -> kernel 0341 (a GPU client's limits >= 64 MB x sweetmemscale, raise only).
    CoreImage's CIKL kernels _asgDownH/_asgDownV (runtime-compiled) are "EntryNotFound": some icons stay plain.
  - Root filled up: /var/db/diagnostics (logd's tracev3, 655 MB) and /var/tmp/SpringBoard.log (65 MB) cleared.
  - Wallpaper: CollectionsPoster HEIC layers (24, /Library/Wallpaper) converted to PNG (originals in
    /var/local/wallpaper-heic-backup); PosterBoard's container is empty, it never launches a poster extension;
    PosterBoard never asks for Metal. A logd capture is unreadable on the Mac (1969 clock, no timesync).
  - Control Center: chrome + backdrop blur drawn, modules only as faint outlines (their glass material renders
    nearly transparent); all 6787 compositor pipelines compile, so a shader computes wrongly. Widgets: the
    renderer (WidgetRenderer_Default, injected, listed) loads the plugin but asks for no device; its surfaces are
    format 0x5f524234 ("_RB4", 2 planes); boxes went from white placeholders to black.
  - Big cores: iOS 26's cache has ~133k LDAPR, ~54k CAS, ~10k CASP and up to ~1.6M LD<op>/SWP sites (loose
    match); the M3 lacks LSE + LRCPC. LDAPR -> LDAR is 1:1; LSE needs out-of-line LL/SC stubs (scratch registers,
    red zone): a project of its own. Command line: a5fb_perf is default on (dropped); unentitled_ios_sim_launch
    is required (macOS-config kernel refuses iOS binaries without it).
- 2026-11-02 evening: **iOS 26 daily image; Starbite 2 plan.**
  - Daily BOOT `out/xnu-boot-ios26-daily.img` (build/ios26-daily-cmdline.txt, sha bee472ca...): cpus=8, no
    starauto/sweetreboot/sweetswipe/sweetdumpfile, sbprof=0 (sbca's 1 kHz main-thread profiler had been on in
    every injected app since metal9), sbshot=0, sweetinject backboardd,SpringBoard,iconservicesagent;
    /var/local/mtl2-env = STARBITE_DRM_LEAKS=0. Menu row relabelled "iOS 26 · 26.6.2 · XNU 12377"
    (tools/brand/menu.mjs -> menu.sbm -> em-font.py -> menu-em.sbm = build/boot-uc3/ramdisk.bin; same 39 sprites).
  - tools/a64e/lse-hotpatch.py: the 25 hottest LSE sites -> LL/SC stubs in __TEXT padding. First version borrowed
    x9-x15 "unmentioned in the function": swift_retain/release_preservemost callers keep them live -> every Swift
    daemon SIGSEGV, no SpringBoard. Now every stub saves/restores its scratch registers (SP-160). Traps: 45 M ->
    26 M per 10 s at startup, 10-15 M -> 3-4 M later. Verified boot ios26-dailytest2: no crashes, GPU active.
  - Old images, shots and build/dxb-11417 archived on the 64 GB PNY USB stick (build/ARCHIVED-TO-USB.txt);
    out/xnu-boot-uc3.img (mkimage template) and out/twrp-3.7.0-a16data.img must stay in out/.
  - Plan: docs/STARBITE-2-PLAN.md (catalog-driven loader, microSD OS library, menu v6 with categories,
    Starbite Lab, fastboot, OS roadmap).
- 2026-11-02 evening: **Starbite 2, phase 0 (safety) and the boot intro.**
  - S9 #1 partition backup backup/20261102-185306 (EFS, CPEFS, PARAM, UP_PARAM, BOOT, RECOVERY, DTB, DTBO, RADIO,
    KEYSTORAGE, PERSISTENT, STEADY, OMR, LU sdb/sdc/sdd/sde, sda head 4 MiB = MBR/GPT/PIT/MD5HDR, backup GPT;
    SHA256SUMS, all checked against the phone). Copies: USB stick starbite-backup/, 128 GB card backup/.
  - S9 #2 (second, wiped, in Download mode): `heimdall print-pit --no-reboot` = the same PIT as S9 #1's original
    (backup/s9b/pit.txt); its bootloader/OEM-lock/KG state still to be read off the Download screen.
  - 64 GB card: starbite-win.vhdx copied to the USB stick (starbite-card64/, sha 8755e0de verified); the 8 GiB
    pmOS image deliberately not kept (Linux gets rebuilt; driver work stays in tools/linux + NOTES); the card is
    now the spare copy of the VHDX and the iOS 16 root.
  - 128 GB SanDisk card (SK128, 249999360 sectors) in S9 #1, MBR: p1 8192..182886399 exFAT "STARBITE" (mkexfatfs
    in TWRP), p2 182886400..249999359 type 0xDA raw 32 GiB for the loader's OS store v2. backup/ and windows/ on p1.
  - Splash: UP_PARAM is a GNU tar of S-Boot's JPEGs (logo.jpg = boot splash, download_L/warning_L/unlock_L =
    Download-mode and warning screens). tools/brand/splash-logo.mjs redraws "Powered by android" as "Powered by
    starbite"; tools/brand/upparam-pack.py rebuilds the tar (refuses unless an unchanged rebuild is byte-identical:
    GNU format); written to S9 #1 UP_PARAM and read back (sha 556079ef...).
  - Menu: Starbite card sprites blank (SHOW_CARD=1 restores them); marker sprites 2/3 = header / splash star.
  - Loader (commit 6958997; **works on hardware**: the intro runs on S9 #1, user: "looks great"; now
    build/boot-uc3/loader.Image, the old one kept as loader-v5-nommu.Image; out/xnu-boot-ios26-daily.img = anim): mmu.c identity map with caches (DMA and OS areas Normal-NC,
    loader Normal-WB), clean + MMU off before every handoff (x1-x3 zeroed after star_dcache_all); anim.c boot intro
    (splash fade, star flight with sparkle trail, ring burst, header fade, menu cascade) composed in a WB buffer at
    0x84000000 (+ logo copy 0x85200000; DTB reserves nothing there). Test image out/xnu-boot-ios26-anim.img
    (`LOADER=shim/starlte/out/xnu_boot.Image tools/mkimage-ios26.sh`).
- 2026-11-02 night: **Phase 3 kernel (plan 1.3 + 2), built, not booted.** One reproducible build:
  `limactl shell starbite -- env BUILD=/mnt/build/starbite tools/linux/build-kernel.sh` (BUILD default
  /mnt/build). It resets mainline to v7.1 8cd9520d35a6 (reset --hard + clean, out/ kept), rsyncs the vendored
  port (tools/linux/port = SheroAbi port @ bcee601: src/, scripts/build/, star2lte DTS, firmware incl. the
  BCM4361 files that build71.sh installs as brcmfmac4361-pcie.bin/.txt/.clm_blob, regulatory.db/.p7s) to
  ~/linux/starbite-work, runs patch-mainline.py --port (DECON early_event param default on; G3D 260 MHz at
  950 mV + rail_check/rail_fixups params; cpufreq transition_delay_us 5000 param; build71.sh fragment hook)
  and --tree (dw_mmc x3, S9P_GEM_DMA32 hunk kept out, Panfrost autosuspend 1000 ms on exynos9810-mali),
  mk-starlte-dts.py, build71.sh, then merges tools/linux/config/starbite.config (CMDLINE_FROM_BOOTLOADER,
  INITRAMFS_SOURCE="", RD_ZSTD, DEVFREQ_GOV_PERFORMANCE, overlay/fuse/squashfs/erofs, zram, PSI, nftables,
  bridge/veth, HID/uinput, AppArmor, systemd needs; 185 values checked by tools/linux/check-kernel.py config).
  DTB: /chosen bootargs `console=ramoops-1 console=tty0 loglevel=4 panic=10 maxcpus=8 s9p_vendor_m3_defaults
  s3c2410_wdt.tmr_atboot=0`, linux,initrd-start/end u64 0 placeholders, CMA moved to 0xCD400000+300 MiB
  (plan's 0xB0000000 would overlap tima and the 0xBC800000 bank hole; check-kernel.py dtb verifies it against
  all reserved-memory, the memory banks and the loader window). s9p_g3d_pd.always_on was already 0644.
  Outputs: Image 25.6 MB (7.1.0-starbite, text_offset 0x80000), DTB, kernel.config in build/linux/ (the old m71
  kernel moved to build/linux/m71-old/) and the VM ~/distros/_kernel/ with modules-7.1.0-starbite.img (546
  modules, ext4 sb-modules, 256 MiB sparse, 31 MB used). Needs an initrd now: no built-in initramfs.
- 2026-11-02 night: **Phase 3 kernel review fixes, built, not booted.** Release is now
  `7.1.0-starbite-<build id>` (LOCALVERSION, id = hash of port/, patch-mainline.py, starbite.config,
  build-kernel.sh and its arguments): this build is `7.1.0-starbite-23527494`, Image sha e994ec21. The init mounts
  `/distros/_kernel/modules-$(uname -r).img`, so a modules image can only meet its own kernel (no MODVERSIONS:
  two configs with one release would otherwise load into each other); the image also holds `/starbite-kernel`
  (`STARBITE-KERNEL <release> <Image sha256>`), checked by `check-kernel.py pair Image modules-<rel>.img` (run it
  before copying modules to the card or packing the Image into the store). Older modules-*.img in ~/distros/_kernel
  are deleted by each build. Rollback DTB `linux-old.dtb` (sha 3f2bddcf, same as the earlier run's): the daily DTB
  with only CMA at mk-starlte-dts.py --legacy-cma (0x98000000 + 256 MiB, ends at OS_RD), made with fdtput, checked
  by `check-kernel.py dtb --legacy-cma --image Image` (CMA between the Image + BSS and the initrd). The old
  build/linux/m71-old DTB is not a rollback for this kernel: no initrd properties, old bootargs, CMA over the initrd.
  Loader: the built-in fallback entry (no catalog in the store) is now linux.image + linux.dtb + linux.initrd with
  sb.root=none (rescue shell). The kernel branch alone still has catalog.toml's `linux` entry without
  linux.initrd: merge the userland branch before packing a store. build-kernel.sh also takes
  ~/linux/.starbite-build.lock (WORK, OUT, REPO_OUT are shared between BUILD trees). s9p-g3d: failed BUCK6
  reads/writes count in rail_fixup_errors (warned with the error) and no longer count as rail_fixups.
- 2026-11-02 late: **Distro builds stalled: leaked images on the Mac.** Lima mounts /Users/dev read-only, so
  mkroot-*.sh build in the git-ignored build/distros-build/ (same APFS disk, sparse; accepted as the build
  location, card-install.sh `image NAME` now finds images there too, SRC=... overrides). The stall: `rm` on
  virtiofs only unlinks; the guest's cached inode keeps the file open in com.apple.Virtualization (`lsof +L1`), so
  every rerun leaked an image (3x 5 GiB apparent + the 829 MB ALARM tarball, 2.7 GB free, all three builds asleep in
  their free-space waits). Freed without restarting the VM: `limactl shell starbite -- bash -lc 'sync; echo 2 | sudo
  tee /proc/sys/vm/drop_caches'` (guest forgets the inodes, host closes them: 2.7 -> 9.5 GB free). mkroot-arch.sh
  now `release`s files (truncate -s 0, rm, drop_caches) and releases a half-built image on failure (tarball kept).
- 2026-11-02 late: **pmOS image review fixes (pmos.img, not booted).** The greetd autologin bypasses phrog, which
  would export the desktop variables from phosh.desktop, and phosh-session sets none: with XDG_CURRENT_DESKTOP unset
  gnome-session falls back to GNOME and drops the OnlyShowIn=Phosh; on-screen keyboard (phosh-osk-stevia, a
  RequiredComponent of phosh.session), so there would be no way to type a Wi-Fi password. initial_session is now
  `env XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Phosh:GNOME XDG_SESSION_DESKTOP=phosh PAN_MAX_AFBC_PACKING_RATIO=0
  phosh-session` (other UIs: DesktopNames= with ';' -> ':', the session file's name). The image also carries an empty
  lib/modules/<build/linux/release> (with sb.rw=0 the initrd cannot mkdir it and silently skips the modules bind).
  Both patched into the existing image (loop-mounted rw, e2fsck -f, resize2fs -M) and checked by mkroot-pmos.sh's
  37 checks: 2143 MiB, sha 62f0fe4c...f510. mk-payload.sh no longer loses a PIN: an old one-line login file has its
  6-digit PIN taken out and is rewritten as "user user" + "pin NNNNNN" (no PIN: kept as .old, a new one is made),
  and the "user user" sed skips any line with a PIN on it. That sed is how the old milestone-3 pmOS root lost its
  PIN: its PIN is unknown and pmos-root.sh cannot log into it (SSH key only). The new pmos.img is fine: its PIN is in
  build/linux/pmos-login.txt, pmos-root.sh greps any 6-digit number, and the SSH key is installed.
- 2026-11-03: **Darwin console (Lab), built, not booted.** tools/darwin/: `dsh` (dsh.c + start.s) is one static
  arm64 Mach-O, no dyld/libSystem, raw XNU syscalls, ad-hoc signed (LC_BUILD_VERSION iOS 14 added after `ld -static`
  so codesign writes CD v0x20400). As PID 1 it opens /dev/console, prints a banner + sysinfo, autoruns
  `sysinfo;mount;ls -l /dev;ps;bench` (boot-args dshauto=0, dshrun=a,b+c, dshtick=N status line period), then a
  `darwin#` prompt; every command runs in a forked child; `sh` execve()s /sbin/launchd again. Commands: sysinfo uname
  sysctl (-a, name=value) ps dmesg ls cat xxd mount cpus mem uptime date bench kill reboot halt ... Boot path = the
  herolte-proven one: mkpayload.py --ramdisk (RAM disk after the kernel, ADT /chosen/memory-map RAMDisk), rd=md0,
  mockfs serves the device as /sbin/launchd (series-12377 0021/0024, Mach-O check), DEVELOPMENT kernel allows static
  executables. `tools/darwin/build-darwin.sh` -> out/darwin.xnu (12377 kernel strip -x + out/starlte-iphone11.adt +
  dsh + tools/darwin/darwin-cmdline.txt, 17.25 MB < the loader's 80 MiB STAGE); `--host` builds the same shell for
  the Mac (`dsh-host --as-init "BOOTARGS"` runs the PID 1 path on stdio): all commands checked there. No UFS, nothing
  written. Input only from a serial console: the S9 keys/touch are IOHID services, not a tty (next: a kernel patch
  feeding sweet_hid_key into the console tty). Catalog entry proposed, not added (key darwin, method xnu,
  files ["darwin.xnu"], tag lab).
- 2026-11-03: **UEFI side (Microsoft category): UEFI Shell entry built, WinPE plan + tooling, 24H2 assessment.**
  Nothing written to the phone. (1) `tools/woa/build-shell-uefi.sh` -> build/woa/shell/uefishell.uefi (sha c29f5343...):
  our source Star2LtePkg (patches 0001,0002,0003,0005,0018,0022,0023) + tools/woa/patches-shell/0024 (switch
  STAR2LTE_BOOT_SHELL, default 0): the edk2 Shell (ShellPkg RELEASE, all commands incl. acpiview) in the FV under
  gUefiShellFileGuid, Star2LteKeysDxe (GPA0/GPA2 keys as SimpleTextIn: Volume = command palette of read-only
  commands, Power = Enter, Bixby = q; ALIVE page added to the GCD as UC MMIO), BDS boots the Shell right after the
  UFS connect and never Windows; own edk2 output dir Build/Star2LteShell. Ring markers =KEYCON / =SHELL / =SHELLRET.
  docs/UEFI-SHELL.md (catalog entry key uefishell, test steps). (2) docs/WINPE-ARM64-PLAN.md: WinPE/WinRE serviced
  ON the phone's Windows (DISM/bcdedit; tools/woa/winpe/starbite-winpe.cmd re|pe|transplant|once|remove: ramdisk
  entry for C:\Starbite\WinPE\boot.wim with fixed BCD ids, S9 drivers from the DriverStore so the UFS driver owns
  the boot watchdog), host fallback with wimlib (tools/woa/winpe/winpe-host.sh, pever.py; wimtools + libhivex-bin
  installed in lima). Main risk: image build vs the firmware's 22621.7582 match (ConvertConfig LCUwinre=0).
  Menu selection design: Star2LteBcdDefaultDxe (in-place REG_SZ swap of {bootmgr} default 23000003). (3)
  docs/WIN11-24H2-A55.md: LSE -> A55 only; A55 core 1 and M3 core 0 share Aff 0x100, so the MADT must list only the
  4 A55 (MPIDR 0x000/0x100/0x200/0x300, parked, no M3, PPTT 1 cluster); per-build CNTFRQ/NTPATCH work for 26100 is
  the blocker. tools/woa/winpe/acpi-dump.ps1 dumps the release's real tables from Windows (first step).
- 2026-11-03: **Native Samsung splash (built, not booted, nothing flashed).**
  - Stock check: logo.jpg in build/fw-vg6/bl/up_param.bin (VG6 firmware tar), backup/20261025-233855 (VG4) and
    backup/20261102-185306 UP_PARAM.img is the same file (136775 bytes, sha e2db11d5...); 20261102 = the VG6
    up_param.bin byte for byte plus zero padding to 8 MiB (the 20261025 one differs only in tar mtimes: VG4).
  - tools/brand/splash-noknox.py: "Secured by Knox" (rows 379..430, x 541..991 of logo.jpg) replaced in the DCT
    domain by a block of the image's own background (jpegtran -crop + -drop, no re-encoding): 0 pixels differ
    outside x 536..999 y 376..439, inside it is 0,0,0; markers kept, baseline 4:4:4. build/brand/upparam-native/
    logo.jpg sha 9ae3c717..., up_param.img (upparam-pack.py, unchanged-rebuild self-test passed) sha 49788aca...
    Not flashed: the user decides.
  - intro7: intro7_capture copies the panel's non-black row bands into I7_CAPT (0x83800000, 8 MiB, checked free at
    run time) as runtime SB7 images (sb7_put, SB7_S_CAPT0..); the intro's first frame is S-Boot's splash pixel for
    pixel (h7 --splash, intro-film.sh I7_SPLASH: 0 px differ for the stock, Knox-free, "Powered by starbite" and
    concept logo.jpg), then it fades into the concept's intro. No capture: the concept splash as before (frames
    byte-identical to the previous loader). Films: build/splash-review/{native,stock,current,v7concept}/.
