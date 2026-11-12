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
| ext4 (Linux roots) | superblock | state (clean / errors), error count and the last error, needs_recovery (not cleanly unmounted), mount counts and times, label, UUID, the metadata_csum checksum (crc32c) |
| HFS+ / HFSX (Apple roots on microSD) | volume header (+ the alternate at the end) | unmounted-cleanly and inconsistent bits, journaled, block counts, last mounted by, dates |
| APFS (iOS 27 on USERDATA) | container superblock (block 0) and the newest checkpoint superblock | Fletcher-64 checksums, block size and count, checkpoint layout, volumes |
| Store | the store's directory and settings record | entries, CRCs, settings status (the store tool already does this) |

Since 2026-11-12 every per-OS page runs the matching check on that OS's root from the catalog's `root` field:
`sd:lba A+N` (a card range), `ufs:NAME` (a GPT partition). `fs_health` sniffs FAT32 / NTFS / ext4 / HFS+ / APFS.
Host test: `sh tools/menu/fwtest/run.sh [IMAGE...]` (36 checks, 3000 fuzzed images).

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

## 10. Starbite OS as the boot menu: the ABI (v1, 2026-11-12)

The user's direction: Starbite OS replaces the boot menu. The OS chooser, the SBFW settings and the recovery UI live in
Starbite OS. The loader stays the minimal safety net (watchdog, crash return, a key to the plain menu, Starbite
Recovery, fastboot). Both sides use the formats below; the loader honours them whoever wrote them.

### 10.1 Shell mode (who starts first)

The settings record (the HOME store's block 2, card p2 after the store move) gets the SBFW part at bytes 112..255:

```
112 "FW" (u16 0x5746)   114 u8 version 1   115 u8 fw_flags (bit 0 SHELL: start the shell instead of the menu;
                                                             bit 1 console never, bit 2 console always)
116 u8 req_action (SBFW_REQ_*)  117 u8 req_state (0 none, 1 armed, 2 applied, 3 undone, 4 refused)  118 u16 req_seq
120 char req_key[24]   the catalog key the one-shot is for
144 char shell_key[24] the shell's catalog key ("" = "starbite-os")
168 u8 undo[64]        a write action's undo record (6.1): u8 part_id, u8 len/4, u16 offset, u32 lba, u32 crc_old,
                       u32 crc_new, the old bytes (<= 48)
232 u8 reserved[24]    zero
```
A record without "FW" at 112 reads as all zero (no shell, no request): every older record.

With SHELL set, on a normal start (no boot-next key, no crash to show, no Bixby, no Volume Up) the loader runs POST
and then **starts the shell entry instead of drawing the menu**. The plain menu comes back:
* **Volume Down held** during the boot (the key that already skips POST once): this boot only.
* the mailbox key **`menu`** (`sb_boot_next("menu")`): this boot only, e.g. "Advanced boot menu" in Starbite OS.
* the shell's own last start **crashed or hung** (crash.c judges it): the crash screen, then the plain menu, and the
  shell is not started again until the next clean start.
* the shell row is missing or does not load: the plain menu, with the reason.

### 10.2 The loader to the OS: the SBFW table (0x91FFE000)

Written right before every jump (every OS, not only the shell), after the launch record. 4 KiB, little endian:
```
0    "SBFWTBL1"   8 u32 version 1   12 u32 CRC32 of bytes 16..4095
16   u32 debug_level 0..3           20 u32 req_action (for THIS start, 0 none)    24 char key[24] (this start)
48   u32 console_y0, console_y1     56 u64 fb_base   64 u32 stride   68 u32 fb_fmt (0 = BGRA8888)
72   u32 loader_version             76 u32 boot_reason (SBFW_WHY_*: 0 menu pick, 1 default/auto, 2 boot-next,
                                                        3 shell mode, 4 retry after crash, 5 fastboot)
80   u32 store_medium (0 UFS, 1 card)  84 u32 store_flags (bit 0 HOME)  88 u64 store_base (4 KiB blocks)
96   u64 store_capacity              104 u32 last_crash_kind (crash.c kind, 0 none) 108 u32 last_crash_sev
112  char last_crash_key[24]        136 u32 post_fails, post_warns   144 u32 settings_status   148 u32 flags
                                                        (bit 0 card only, bit 1 shell mode on)
128.. reserved up to 1024
1024 char last_lines[24][120]       the console's last lines at hand-off (oldest first, NUL padded)
```
Starbite OS reads it first thing (it lies below its load address 0x92000000, beside the loader's log page
0x91FFF000 it already reads). It tells the OS why it runs, where the home store is (no probing, no UFS guess), what the
last start did, and what to show on its recovery page.

### 10.3 The OS to the loader

| Want | How (libsb) | Loader |
|---|---|---|
| start an OS once | `sb_boot_next(KEY)`: mailbox 0xFED17FE0 "SBNEXT2!" + key, warm reset | starts KEY (existing) |
| start an OS, come back after | "SBNEXT2R" KEY\0RET (existing, docs/MAINTENANCE.md R7) | starts KEY, arms RET for its reset |
| the plain menu once | `sb_boot_next("menu")` | **new**: menu, shell skipped this boot |
| Starbite Recovery | `sb_boot_next("recovery")` | **new**: Starbite Recovery |
| Rescue / Maintenance / loader fastboot | `sb_boot_next("rescue" / "maint" / "fastboot")` | existing |
| change a setting (default OS, timeout, debug level, POST, watchdog, crash action, shell on/off) | read the record, change, write block 2 of the HOME store, read back (libsb `settings_save`, the codec in sbfmt) | reads it at its next start |
| a one-shot for an OS (safe mode, single user, previous kernel, verbose once, Advanced Startup) | write `req_action/req_key`, `req_state = 1 armed`, `req_seq + 1` into the record, then `sb_boot_next(KEY)` | applies it to that start only, `req_state = 2`; at its next start runs the undo (writes) or just clears it (command-line requests), `req_state = 3` |

The OS never writes anything else of the loader's. The record's CRC (bytes 16..255) covers the SBFW part.

### 10.4 One-shot actions (SBFW_REQ_*)

| # | Name | For | What the loader does for that one start |
|---|---|---|---|
| 1 | `verbose` | any | the debug level's Highest fragment instead of the configured one |
| 2 | `safe` | bare (Starbite OS, Utility, Engineer) | table req_action = 2; the OS starts with no apps and writes no settings |
| 3 | `single` | Linux | `single` (and `systemd.unit=rescue.target`) appended |
| 4 | `prev` | Linux | the kernel `linux-prev.image` (or `<file>.prev`) instead of the entry's |
| 5 | `xnu-single` | XNU | boot-arg `-s` |
| 6 | `winre` | Windows | BCD default -> WinRE for one boot: **plan only** (compiled out, 6.2) |
| 7 | `driver-off` | Windows | table req_action = 7; our drivers stay off (needs the UEFI to reserve the table) |
| 8 | `no-shell` | the shell | the next start shows the plain menu (same as the mailbox key) |
