# Starbite firmware layer (SBFW): design

Status 2026-11-11: design, plus the first features in the loader (§9). The user's goal is one firmware layer under
every OS. It should show a debug view on part of the screen when debugging is turned up, and offer per-OS options
and repairs: Windows into Advanced Startup, emergency repairs, file-system health, and the same idea for every OS.

Everything here lives in the loader (`shim/starlte/`). It runs before any OS and reads or writes only what it names,
with dry runs first, as everything else in Starbite.

## 1. What it is

Four pieces, one model:

| Piece | What it does | Where |
|---|---|---|
| **Settings** | Global (debug level, POST, watchdog, crash screen) plus per-OS options and one-shot requests | the store's settings record (§2) |
| **Debug console** | A screen region the loader keeps for its log; at High / Highest it stays visible through the menu and the OS start | the panel's framebuffer (§4) |
| **Health tools** | Read-only checks of each OS's partitions and file systems, and of the store | the loader, on demand from the per-OS page (§5) |
| **Per-OS actions** | One-shot boot changes (Advanced Startup, safe mode, a previous kernel, a driver off for one boot), each with a dry run, a confirm, and an automatic undo | the loader writes them; §6 says per OS how the OS sees them |

The UI is one **per-OS page**, opened from the boot menu: Hold Power on a system → *Firmware options*. It shows that
OS's options and tools, the debug level, and the result of each action. The menu itself is unchanged.

## 2. Settings model

The store's settings record (block 2 of the UFS store, `sbfmt.h`) already holds the global settings. Its CRC covers
bytes 16..255, and `settings_save` keeps bytes 112..255 it does not know of. The firmware layer uses that space:

```
112  "FW"                     (u16 magic of the SBFW part)
114  u8 version 1
115  u8 debug_split            0 auto (by level), 1 never, 2 always
116  u8 req_action             one-shot request, SBFW_REQ_* (0 none)
117  u8 req_state              0 armed, 1 applied (the loader changed the OS's files), 2 undone
118  u16 req_seq               bumped on every new request
120  char req_key[24]          the catalog key the request is for
144  u8 undo[96]               what the loader must put back (§6.1: partition, block, offset, length, old bytes' CRC)
240  u8 per_os_flags[16]       reserved: per-OS bits keyed by catalog index (safe-mode default, verbose for one OS)
```

* **Debug level** is the existing log level (flags bits 8-9), renamed in the UI: Off = QUIET, Normal = NORMAL,
  High = VERBOSE, Highest = DEBUG. Every catalog entry already has four `log` command-line fragments (one per level),
  so each OS already gets its own verbosity from the same setting.
* **Per-OS options** that change how an OS starts persistently stay in the catalog (`cmdline`, `log`, `wdt`), which the
  host tools own. The settings record holds only what the user flips on the phone.
* **One-shot requests** live in the record (`req_*`) so they survive the boot that applies them. The loader clears
  them, and runs the undo, on its next start (§6.1).

How an OS reads the settings, by kind:

| OS kind | Gets | How |
|---|---|---|
| Linux, Android (our kernels) | debug level, requests | the command line (already: `log` fragments), plus `sbfw.level=N sbfw.req=NAME` |
| XNU (iOS, macOS) | debug level | boot-args (already: `log` fragments with `!name` removals) |
| Windows (UEFI) | debug level, requests | an **SBFW table**: a 4 KiB page at a fixed address (§3), which our UEFI's DXE can turn into ACPI or variables later |
| Starbite OS, Utility, Engineer (bare) | everything | the SBFW table, read by libsb |

## 3. The SBFW table (handoff to the OS)

A 4 KiB page in RAM that nothing else uses at handoff: **0x91FFE000** (the page below the loader's log page
0x91FFF000; both lie below RAM_BASE and outside every OS load area). The loader writes it right before every OS
start, after `launch_go` (watchdog) and before the jump:

```
0   "SBFWTBL1"   8  u32 version 1   12 u32 CRC32 of bytes 16..4095
16  u32 debug_level (0..3)          20 u32 req_action        24 char key[24]
48  u32 console_y0, console_y1      (the panel rows the loader's console holds; 0, 0 = none)
56  u64 fb_base, u32 stride, u32 fmt
72  u32 loader version, build stamp
128 char last_lines[16][120]        (the console's last lines at handoff, for the OS's own log)
```

Linux also gets it as `sbfw.table=0x91ffe000`. Under XNU the page is outside XNU's RAM window, so XNU leaves it
alone. The woa UEFI must reserve it (an EfiReservedMemoryType range) before it can pass it on: that is UEFI work, see §8.

## 4. Debug console (split screen)

* **Off / Normal:** as today. The menu is full screen, and loader messages are muted under menu v7.
* **High:** the bottom **20 %** of the panel (rows 2368..2960) becomes a console. It shows the newest lines of the
  loader's log (the pmsg ring at 0xFED14000, which the loader, POST, crash detection, SMP and USB already write),
  one line per row, newest at the bottom, in the mono font on black.
* **Highest:** the bottom **40 %** (rows 1776..2960), plus the loader's own status lines while an OS loads.

The console is drawn after every menu frame, so the glass renderer never covers it. During the launch card
(progress, handoff) it keeps updating with each loader stage, and it stays on the panel at the jump. What happens
next, by OS:

| OS | The console after the jump |
|---|---|
| Linux, Android (simplefb) | **later:** the DT's simple-framebuffer height is cut to console_y0, so the kernel's fbcon and the OS stay above it, and our kernels' earlycon writes into the console rows (`sbfw.table`) |
| XNU | **later:** sweetfb takes `sbfw_console=y0`; its kprintf-to-screen (verbose levels) writes there instead of over the UI |
| Windows | the woa UEFI repaints the panel (logo), so the console lasts until then; **later** the UEFI's DXE prints its ring (=NTPATCH, =EBSOK ...) into the console rows until ExitBootServices |
| Starbite OS / bare | libsb can keep the rows as a log pane (it already draws its own log at High) |

So the first version is the loader's own console, kept up to the jump. Keeping it alive *inside* each OS is per-OS work
(the "later" rows), and every one of those reads the same SBFW table fields.

## 5. Health tools (read-only)

All of these only read. They run on demand from the per-OS page, and none mounts or writes anything.

| Check | What it reads | What it reports |
|---|---|---|
| GPT | LU0's partition table (`gpt_load`) | the OS's partitions found, sizes, overlaps |
| FAT32 (WINESP, EFI) | boot sector, FSInfo, FAT[1] | signature, cluster size, **clean-shutdown and hard-error bits**, free clusters |
| NTFS (WINDOWS) | boot sector, $MFT records 0-3, $MFTMirr | signature, cluster size, version, **$Volume dirty flag**, label, MFT vs mirror match |
| BCD hive (Windows) | `\EFI\Microsoft\Boot\BCD` on WINESP | `regf` header checksum, **sequence numbers (clean / dirty)**, the default entry, its recovery sequence (WinRE) and whether that exists |
| ext4 (Linux roots) | superblock | state (clean / errors), errors count, last mount and last check (**later**) |
| HFS+ / APFS (Apple roots on microSD / USERDATA slots) | volume header / container superblock | the journal and clean-unmount bits (**later**) |
| Store | the store's directory and settings record | entries, CRCs, settings status (the store tool already does this) |

## 6. Per-OS actions

Each action has the same life cycle: **plan** (read-only: what would change, where, from what to what) → the user
confirms → **apply** (the loader writes) → the OS boots once with it → **undo** (the loader's next start puts it back).
The plan screen is the dry run, and it can be used as often as wanted without writing anything.

### 6.1 One-shot and undo

The record's `req_*` fields (§2) carry the request across the reset. The undo runs at the **start of the loader's next
boot, before the menu**, whatever happened in between (the OS hung, the user forced a restart, it booted fine). An
action that changes another OS's files records exactly what it wrote (partition, 4 KiB block, offset, length) and the
CRC of the old bytes. The undo writes the old bytes back only if the block still holds the new ones. If the OS changed
the block meanwhile, the loader leaves it alone and says so.

### 6.2 Windows

| Action | How | Risk / status |
|---|---|---|
| **Advanced Startup next boot** (WinRE) | One boot with BCD `{bootmgr}` default = the Windows entry's `recoverysequence` (WinRE). The two GUIDs have the same length (38 UTF-16 chars), so the change is an in-place 76-byte edit of one value's data in the BCD hive file on WINESP (FAT32). It needs no hive cell changes, no checksum (cells have none; the base block is untouched), and only one 4 KiB block. The loader refuses if the hive is dirty (sequence numbers differ: a log replay could undo the edit). Undo per §6.1. | **WinRE must match the woa firmware's build** (docs/WINPE-ARM64-PLAN.md): the installed Winre.wim is probably not 22621.7582, so WinRE may stop at the logo. The undo brings the next boot back to Windows anyway. **First version: plan only** (dry run); the write is compiled out until the plan is checked against a pulled BCD and the user agrees. |
| Advanced Boot Options (F8 menu: safe mode, debugging, driver signature off, "Repair your computer") | Flip one pre-made boolean element: `bcdedit /set {default} advancedoptions no` once in Windows creates element 16000040 = 0, and the loader flips that byte to 1 for one boot. | Uses Windows' own winload (build matches). Needs the one-time `bcdedit` in Windows, and bootmgr's menu needs keys (volume/power mapping in our UEFI: to check). **Later.** |
| Safe mode one boot | The same pre-made-element trick with `safeboot` (`bcdedit /set {default} safeboot minimal` creates it; the loader keeps it disabled by swapping the element's key name in place?). | Not in place without a pre-made disabled form; **design open**. |
| Disable our GPU / display driver for one boot | Our drivers already honour kill files (`Windows/Starbite/sbmali.off`). That is an NTFS write, which the loader can't do. Instead our drivers read the SBFW table's `req` (§3) and stay off. | Needs the table reserved by the UEFI and a line in each driver. **Later.** |
| Registry rollback from our backups | Host-side today (TWRP + `tools/win/hive-*.py`). In the loader it would need NTFS writes. | **Not in the loader**; the per-OS page can show which backups exist in the store. |
| Disk health | §5 (GPT, WINESP FAT32, WINDOWS NTFS, BCD). | **First version.** |

### 6.3 Linux (our kernels)

Single-user (`sbfw.req=single` → `systemd.unit=rescue.target` / `single` appended for one boot), the previous kernel
(the store's `linux-prev.*` entries, the `linux-pmos-prev` row today), and an ext4 health check (§5). All of these
are command-line changes for one boot: nothing is written, so there is nothing to undo.

### 6.4 Android (One UI)

Boot to TWRP (INFORM3 recovery, exists) and a verbose kernel for one boot (`log` level). Nothing else: Samsung's
kernel and vendor own the rest.

### 6.5 Apple (XNU)

Verbose (`-v`, already the High/Highest `log` fragments) and single-user (`-s`) for one boot, both boot-arg changes.
HFS+/APFS health later.

### 6.6 Starbite OS / Utility / Engineer

Safe mode (`sbfw.req=safe` in the table: no apps, no settings writes). libsb reads the table.

## 7. Safety rules

* Read-only by default. Every write is one 4 KiB block of a named partition, at a computed offset, with the old bytes
  checked first and read back after. Each write type is compiled in only once its plan has been checked on the phone.
* Never: EFS, CPEFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA, DTB, DTBO, GPT, RECOVERY (the same list as everything
  else).
* The undo runs before the menu on every boot that finds an applied request, so no state change outlives one boot of
  its OS.
* Every action screen says exactly what it will change, and nothing is written without Power to confirm.

## 8. Who does what

| Part | Owner |
|---|---|
| Loader: settings fields, SBFW table, console, per-OS page, health checks, BCD plan | boot-menu session (this doc) |
| woa UEFI: reserve 0x91FFE000, print the DXE ring into the console rows | Windows UEFI owner |
| Our Linux kernels: simplefb height, earlycon into the console | kernel owner |
| sweetfb: `sbfw_console` | XNU owner |
| libsb: read the table (safe mode, console pane) | Starbite OS owner |

## 9. First version (2026-11-11)

* **Debug level** on every per-OS page (Off / Normal / High / Highest = the log level); the page saves the settings
  record.
* **Debug console** in menu v7 at High / Highest (§4), kept through the launch card up to the jump.
* **Windows page:** *Disk health* (GPT, WINESP FAT32, WINDOWS NTFS, BCD hive), and *Advanced Startup next boot* as a
  **plan** (the BCD change it would make, where, and from what to what). The write is not built in yet.
* Host tests of the FAT32 / NTFS / BCD parsers against this phone's own files (`backup/20261107-win-profile/BCD`,
  `build/woa/ufstest-esp.img`).
