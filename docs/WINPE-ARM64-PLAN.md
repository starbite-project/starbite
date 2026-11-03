# Windows PE / RE ARM64 on the S9: plan and tooling

Status 2026-11-03: tooling written and tested on the host side (synthetic WIM). Nothing has been
written to the phone, and no Windows media has been downloaded.

## The constraint and the way around it

The ADK's WinPE add-on (copype, MakeWinPEMedia, DISM servicing) runs only on Windows, and the UTM
Windows VM was deleted to free disk space (NOTES.md 2026-10-28). But **the phone itself runs Windows 11
22631.7584** (desktop, touch, DISM, bcdedit, reagentc all present). So the WinPE image is serviced
there with Microsoft's own tools. On macOS/lima, wimlib handles inspection and a DISM-free fallback build.

What boots it: the existing `windows.uefi` (src38: our SEC/PEI + the release 22621.7582 DXE volume)
starts `\EFI\Microsoft\Boot\bootmgfw.efi` on WINESP as usual. The BCD there gets one more loader
entry: a **ramdisk boot of `C:\Starbite\WinPE\boot.wim` on the WINDOWS partition**. Bootmgr reads
NTFS itself, and `bcdedit` stores `[C:]` GPT-qualified, so nothing new is needed in the firmware.

## The build-match rule (the main risk)

The woa firmware patches winload and ntoskrnl in memory and is matched to one build:
`build/woa/win-bin/winload.efi` is **10.0.22621.7582** (pever.py), the build the release DXE expects
(woa docs/installer/validation.md: other builds "may stop at the Samsung logo"). Our ISO was converted
with `LCUwinre=0 UpdtBootFiles=0` (build/woa/uup/ConvertConfig.ini), so the installed `Winre.wim`
and the ISO's `boot.wim` are most likely **not** at .7582. Three outcomes, in test order:

- **A. Base image as is.** The release DXE contains a module named `Winpe22621RuntimeFrequency`
  (build/woa/relmods): the woa firmware has a WinPE-22621 timer path, so a base-22621 WinPE may just
  work. Check: the ring shows `=NTPATCH n=..` and `=EBSOK` like a Windows boot.
- **B. Transplant (low probability).** `starbite-winpe.cmd transplant` copies this Windows' own
  winload.efi, ntoskrnl.exe and hal.dll (22621.7582) into the image. The kernel then matches the
  firmware, but everything else stays at the base CU: ntdll.dll, ci.dll, the boot drivers and the
  rest of early user mode. A 7582 kernel next to an older ntdll/ci.dll/driver set is likely to
  bugcheck at boot or fail in early user mode (smss/winlogon), so B is a cheap experiment to try
  once, not a fallback to count on.
- **C. A matching image (the realistic route).** Re-run the UUP converter with `LCUwinre=1` and
  `UpdtBootFiles=1`, so DISM applies the same cumulative update to WinRE / boot.wim, or obtain any
  WinPE/WinRE image that is already at **22621.7582** (versions.txt confirms it). This needs DISM,
  so it runs in Windows: on the phone (slow, but possible) or in a temporary VM (about 20 GB, not
  now: the Mac has 12 GB free).

If A fails, plan on C (or a matching .7582 image); treat B only as a quick check on the way.

## Route 1 (primary): WinRE serviced on the phone

WinRE is WinPE plus the touch-friendly "Choose an option" recovery UI (Troubleshoot, Command Prompt,
Startup Repair, System Restore, Reset), which is usable without a keyboard. Our BCD has no recovery
entry (no 0x23000008 element), so WinRE is currently unused.

`tools/woa/winpe/starbite-winpe.cmd` (copy it to the phone's Windows, run as Administrator):

1. `starbite-winpe.cmd re`: copies `C:\Windows\System32\Recovery\Winre.wim` to
   `C:\Starbite\WinPE\boot.wim` and `C:\Windows\Boot\DVD\EFI\boot.sdi` next to it. It mounts the
   image with DISM and adds the three S9 driver packages **from this Windows' DriverStore**
   (`exynos9810*.inf_arm64_*`, `s6sy761*.inf_arm64_*`: the S9-fenced UFS driver installed
   2026-10-27, HSI2C, touch) with `/ForceUnsigned`, as tools/woa/vm/build-image.ps1 did. It writes
   `versions.txt` (winload, ntoskrnl, hal in the image and in C:\Windows) and commits. Then it
   creates two BCD objects with fixed ids: a device-options object
   `{5b1e5d7a-2c61-4f0b-9e2a-53544152524f}` (ramdisksdidevice partition=C:, ramdisksdipath
   \Starbite\WinPE\boot.sdi) and the loader `{5b1e5d7a-2c61-4f0b-9e2a-535441525045}` "Starbite
   WinPE". The loader uses device/osdevice `ramdisk=[C:]\Starbite\WinPE\boot.wim,{options}`, path
   `\Windows\System32\Boot\winload.efi`, `winpe yes`, `detecthal yes`, and the same platform settings
   as our Windows 11 entry (testsigning, numproc 4, nx OptIn, hypervisorlaunchtype off,
   bootstatuspolicy IgnoreAllFailures). It is **not** added to `{bootmgr}` displayorder: with two
   entries there, bootmgr may show its text boot menu for the BCD timeout (30 s by default) on
   every ordinary Windows 11 boot, with no keyboard to answer it, while the release firmware's SoC
   watchdog (about 80 s, armed before bootmgr, disarmed only by Windows' UFS driver) keeps
   running, so a menu wait could turn normal boots into watchdog resets. **The default, the boot
   order (displayorder) and the Windows 11 entry are not touched**; the WinPE entry is reachable
   only through `once`.
2. If versions.txt shows the image is not at 22621.7582: try A first. Then the realistic next step
   is C (a matching 22621.7582 image); `starbite-winpe.cmd transplant` (B) is a low-probability
   quick try.
3. `starbite-winpe.cmd once` sets `bcdedit /bootsequence` to the WinPE entry for one boot
   (bootsequence works for an entry outside displayorder and does not show the menu). Restart
   (the restart goes through PSCI reset, S-Boot and our menu), then pick **Windows 11**. Bootmgr
   then takes the one-shot sequence into WinPE.
4. `starbite-winpe.cmd remove` undoes everything (both BCD objects and C:\Starbite\WinPE).

The `pe` mode does the same with any WIM (for example an ISO's `sources\boot.wim` copied to C:) and
writes a `startnet.cmd` that runs `wpeinit` (plain WinPE command prompt).

Why the drivers must be in the image (boot-start through DISM, not `drvload` later): the release DXE
arms the SoC watchdog before it starts bootmgr (`=PREWDT`, about 80 s), and only the woa UFS driver
takes ownership of it (RWD1, NOTES.md 2026-10-26). Without the driver early in boot, WinPE is reset
about 80 s after the firmware hands over.

## Route 2 (fallback, no DISM): wimlib in lima

`tools/woa/winpe/winpe-host.sh` (lima, with wimtools and libhivex-bin installed in the VM 2026-11-03):

- `info WIM` and `versions WIM [INDEX]`: the image list, and the winload/ntoskrnl/hal file versions
  (tools/woa/winpe/pever.py, a VS_FIXEDFILEINFO reader). Use these on a pulled Winre.wim or an
  ISO's boot.wim before anything else.
- `build ISO|boot.wim OUT [DRIVERS]`: exports image 1 as a bootable LZX WIM (`--boot`) and adds
  `X:\Starbite\Drivers` plus a startnet.cmd (`wpeinit`, then `drvload` of every INF). With an ISO it
  also takes `\boot\boot.sdi`. It writes boot.wim, boot.sdi, versions.txt and SHA256SUMS. Tested
  2026-11-03 on a synthetic WIM: export, update, verify and version read all work.

Limits: `drvload` runs only after wpeinit, which is probably too late for the 80 s watchdog. The BCD
entry still has to be made with bcdedit on the phone (`starbite-winpe.cmd pe C:\...\boot.wim`). The
BCD device elements (ramdisk inside partition) are binary structures; editing them offline without a
reference sample is not worth the risk. Simple elements can be edited offline (see below).

## Getting the files onto the phone (test time, needs the phone)

- Route 1 needs no host files at all: copy `starbite-winpe.cmd` to Windows. Windows has no SD card
  driver and no USB, so either type it in, or put it on the NTFS volume from TWRP: `mount.ntfs` rw
  works when Windows shut down cleanly (NOTES.md 2026-10-27, DriverStore swap), so copy to
  `/WINDOWS/Starbite/`.
- Route 2: push boot.wim/boot.sdi to `C:\Starbite\WinPE\` the same way.
- To read results: TWRP `mount.ntfs -o ro`, then `adb pull` of `C:\Starbite\WinPE\versions.txt` and
  `starbite-winpe.log`.

## Store entry: selecting WinPE from the Starbite menu (design, not built)

`bootsequence` works only from a running Windows. To make "Windows PE" a menu entry, it gets its own
UEFI image: `winpe.uefi` = `windows.uefi` (src38) + one small DXE driver added with
`tools/woa/dxe-replace.py --add`. That is the same technique as the src38 RTC module, already proven.

**Star2LteBcdDefaultDxe** (to write): on the ReadyToBoot event group (signalled by the BDS right
before LoadImage of bootmgfw), it opens `\EFI\Microsoft\Boot\BCD` on WINESP through
SimpleFileSystem (EnhancedFat is read-write). It walks the regf hive
(nk/lf/lh/li/ri/vk, as tools/woa/hive.py does) to
`Objects\{9dea862c-5cdd-4e70-acc1-f32b344d4795}\Elements\23000003\Element`. That value is REG_SZ, a
38-character GUID string; today it is `{a446ff58-d0bf-11f1-8ddc-c89f478c11fc}` = Windows 11. The
driver overwrites it in place, same length, so no cell moves and no hive checksum changes (the base
block is untouched; the hive is clean, seq 49/49). In `winpe.uefi` it writes the Starbite WinPE id;
in a rebuilt `windows.uefi` it writes the first displayorder entry back, so each menu entry gets its
own default every boot. This is the same kind of in-place BCD value edit as the bootstatuspolicy and
numproc tests (NOTES.md 2026-10-26).

Proposed catalog entry (once winpe.uefi exists):

```toml
[[entry]]
key = "winpe"
name = "Windows PE"
version = "WinRE 22621 · rescue"
category = "microsoft"
method = "uefi"
tag = "beta"
files = ["winpe.uefi"]
root = "ufs:WINDOWS \\Starbite\\WinPE\\boot.wim"
```

## Test plan on the phone (in order, each step alone)

1. In Windows: `starbite-winpe.cmd re`. Read versions.txt.
2. `starbite-winpe.cmd once`, restart, pick Windows 11. Expected: the Windows logo and spinner, then
   WinRE's "Choose an option". Touch should work (S6SY761 is in the image).
3. If it resets about 80 s in: the UFS driver did not load (check the starbite-winpe.log Add-Driver
   lines, or the latches `7F40` WATL and RWD1 in the next menu boot's ring). If it stops at the logo
   with no reset: the build-match rule. Go to C (a matching .7582 image); `transplant` (B) may be
   tried once first but will most likely bugcheck or stop in early user mode.
4. Leave WinRE with "Continue" (into Windows) or "Turn off".
5. Only after 2 works: write Star2LteBcdDefaultDxe and the winpe.uefi store entry.

## Blockers

- The WinRE/boot.wim build revision (very likely not .7582): unknown until versions.txt exists.
- No ADK on the Mac. Offline driver injection that is boot-start needs DISM, which runs only in Windows.
- The phone was not available today, so all of this is untested on hardware.
