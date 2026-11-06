# Starbite boot health: full POST, quiet boots, crash and hang detection

Written 2026-11-06 for three requests of the user:

- (a) "the actually proper post sequence like the boot process to boot screen can take like 10s or a
  bit more, but add a way to bypass it, like in the fw menu with bixby key but by default the full
  post is on";
- (b) "cleaner os boots like debug logs off normally except behind flags so it feels more native ...
  but ofc you can easily change debug level";
- (c) "the panic and hang detection like firmware level or whatever runs below all operating systems
  ... the timeout can be adjusted too but its mainly for panics and it shows then varying levels of
  debug info from crashed to everything and it saves".

It also covers the coordinator's request for One UI 8: an Android entry may bring its own device
tree (section 6). It also covers the loader side of Starbite Maintenance (SBMAINT, section 12), and
the shared settings flag bits agreed with Engineer OS v2 (section 7).

**Status (2026-11-07):** the full POST, the crash detection and the crash screen ran on the phone with
`xnu-boot-health2.img` (a real Linux panic was detected and shown). The user's verdict led to the
POST presentation of section 2.2 (the logo, Volume Up for the live log, the failure screen) and the
crash report of section 4.4, both in `out/xnu-boot-health3.img` (section 10): **built and host-tested,
not on the phone yet.** XNU's side (the watchdog
feeder and the crash return, xnu/series-12377/0399) is in section 5.4, and its phone tests in
10.1. The loader builds with
`-Werror` and has no ABS64 relocations. The host tests (section 8) pass with the Mac's clang
(ASan + UBSan) and with gcc in the VM. Section 9 lists what only the phone can confirm. Section 10
is the flash and test plan.

## 1. Files

| File | What |
|---|---|
| `shim/starlte/sbfmt.c/.h` | **No hardware** (host-tested): the settings record codec, the crash log format, the reset-reason decision, watchdog math, ramoops ring text, command line edits, dates |
| `shim/starlte/dtmerge.c/.h` | **No hardware** (host-tested): DTBH selection, the S-Boot merge for Android device trees, in-place DTB cell patch |
| `shim/starlte/post.c/.h` | POST levels, the full POST behind the Starbite logo, the live log (Volume Up), the failure screen, Volume Down to skip |
| `shim/starlte/post_logo.h` | **Generated** by `tools/brand/post-logo.py` from the One mark splash source (`build/splash-concepts/final/logo.png`): the word (A8) and the star (RGB), PackBits, 11 KB |
| `shim/starlte/crash.c/.h` | Snapshot at start, launch record, evaluation, report, crash screen, the recovery menu's report list, fastboot text and raw |
| `shim/starlte/wdt.c/.h` | Arming cluster 1's watchdog right before an OS starts |
| `shim/starlte/xnu_boot.c` | The order (section 4.1), log-level fragments, watchdog arguments, Linux INFORM3 patch, Android 4th file, Starbite Maintenance's start (section 12) |
| `shim/starlte/store.c/.h` | Settings record with the new fields (bytes kept), `crashlog` entry access (`SF_VOLATILE`) |
| `shim/starlte/catalog.c/.h` | The catalog extension `SBCATEX1` |
| `shim/starlte/recovery.c` | "STARTUP SETTINGS...", "CRASH REPORTS...", "STARBITE MAINTENANCE"; the early mailbox keys (rescue, maint*, the `SBNEXT2R` return key) |
| `shim/starlte/menu.c`, `menu6.c`, `menu7.c` | No auto-start countdown right after a crash |
| `shim/starlte/fastboot.c`, `fastboot_dev.c` | `fastboot oem log crash` and `crashraw`, `fastboot oem maint [prev\|rescue]`; `oem boot-next`/`reboot-to` accept the loader's own keys |
| `shim/starlte/fbtext.c/.h` | Glyphs `( ) ' " @ \| ; & $ { } ^ ~ \` \\` for log lines; `star_fb_muted()`; lower case and `star_fb_mini` (the same 5x7 glyphs in 18 x 36 cells: 76 columns, 78 rows a page) for the crash report |
| `tools/store/sbstore2.py` | Settings fields, `settings-show`, `crashlog IMG\|live`, the catalog extension, the `crashlog` store entry |
| `tools/store/catalog.toml` | `[defaults.linux/android/xnu]`, per-entry overrides, field documentation |
| `tools/loader-test/` | Host tests and fixtures (section 8), with real-looking crash captures (`fixtures/linux-oops-panic.txt`, `linux-sysrq-panic.txt`, `xnu-panic.txt`) |
| `tools/brand/post-logo.py` | Makes `post_logo.h` (section 2.2) |
| `xnu/series-12377/0399-starlte-health-wdt-crash-return.patch` | **XNU's side** (section 5.4): `sbwdt=N` feeder, `sbcrash=loader\|recovery`, the crash marker, test switches; `bsd/dev/herolte_ufs.c` + new `bsd/dev/starlte_health.h` |
| `tools/xnu-health-test/` | Host test of `starlte_health.h`, taken out of the patch, against the loader's sbfmt.c / crash.c / hw.h / wdt.h |
| `tools/store/catalog-xnu-health.toml` | Proposal for the coordinator: `[defaults.xnu]`, per-entry watchdog, hidden XNU test entries |

## 2. POST (request a)

### 2.1 Levels and keys

| Settings flags bits 4-5 (`SET_POST_*`) | What runs | Time |
|---|---|---|
| **full** (0, the default; every record older than 2026-11-06 reads as full) | all 16 checks on their own screen | about 8 to 10 s, see 2.4 |
| quick (1) | the 11 checks of 2026-11-05, as lines over S-Boot's splash | about 0.3 s |
| off (2) | nothing; the menu brings UFS and touch up when it needs them | 0 |

- **Change it**: hold Bixby at boot, then **STARTUP SETTINGS... → POST AT BOOT** (Power cycles FULL,
  QUICK, OFF), then **SAVE**. From a Mac: `sbstore2.py settings OUT --from BLOCK --post quick`. Engineer
  OS v2's settings editor writes the same bits.
- **Skip it once**: hold **Volume Down** during POST. The rest of the run is marked SKIPPED, the
  boot goes on, and the setting does not change. Volume Down counts once it is down 20 ms; the keys
  are polled after every check, every 4 MiB of the RAM test and between the timer test's RTC reads.
- **See what it does**: press **Volume Up** during POST: the live log (2.2) for the rest of the run.
  Volume Up **held from power on** is the loader's fastboot gesture (`fastboot_wanted`, which runs
  after POST): POST leaves it alone (the logo stays) until it has been seen released for 40 ms.
- **Bixby** during POST opens Starbite Recovery afterwards, as before.
- The keys are a host-tested state machine (`pv_init`/`pv_poll`/`pv_end`/`pv_hold` in sbfmt.c).
- **The Bixby check comes before all of this and is unchanged**: `rec_bixby_window` still uses only
  the key GPIO, the timer and fbtext. The settings record is read after it, and so is POST.

### 2.2 The full POST: what it shows (2026-11-07)

The user, after the first phone run: "i dont want that progress bar it doesnt do shit ... i just want
starbite logo and so that when you hold volume up it shows the live log, but normally just starbite
logo with some little animation". So:

- **Normally: only the Starbite logo**, the approved One mark splash (`docs/concepts/splash/HANDOFF.md`),
  exactly where S-Boot draws its splash (word at (478, 1327), 455 x 108; star centred at (961.74,
  1259.0)). No text, no bar. The animation: the star breathes a soft #6FB3FF glow (2.4 s, at most
  30 %, the star 87..100 %, dithered so the dim rings do not band) and three small dots under the
  word ripple (1.5 s). Frames are drawn between and inside the checks, at most one every 33 ms
  (about 1 ms each: the glow box is 169 x 169 px).
  - If the panel already shows the One mark (S-Boot's splash with One mark in UP_PARAM: the word
    matches and a coarse grid elsewhere is black), nothing is cleared: the animation simply starts on
    S-Boot's own picture. Otherwise the panel goes black and the logo fades in over 360 ms.
  - The logo is not loaded from the ramdisk: `post_logo.h` (11 KB in the loader) is unpacked to
    0x9B100000 at the start, so it works on every boot path (also the unbranded iOS one).
- **Volume Up pressed during POST: the live log**, for the rest of the run (releasing it does not go
  back: switching views twice would only flicker). Every check with its state as it runs; the RAM line
  shows its own progress: `C: CHECK 512/928 MIB 81%` (window, write or check, MiB of this pass,
  percent of the RAM time). At the end: the summary line and the problems list, kept for 2 s, or as
  long as Volume Up stays held (at most 60 s); Power or Volume Down continue at once. Log level 2-3
  (verbose, debug) starts in the log.
- **A red check: the failure screen** after the run: "A CHECK FAILED" / "N CHECKS FAILED", each failed
  check with its one-line reason (at most 5), "POWER: CONTINUE" ("POWER: STARBITE RECOVERY" for RAM or
  TIMER) and "BIXBY: STARBITE RECOVERY", continuing on its own after 30 s. Volume Up is not a key on it
  (it may still be fastboot's). Gold warnings never interrupt the logo: they are in the log page, the
  pmsg ring and `post_last()` only.
- **The handover to the menu**: when menu v7 will run (`menu7_ready()`), POST leaves the clean logo on
  the panel (glow and dots off) instead of putting S-Boot's splash back. v7's intro then captures it
  exactly as it captures S-Boot's One mark splash and hands it over (the star becomes the wallpaper's,
  the word the island). With the "Powered by starbite" splash still in UP_PARAM, the intro therefore
  starts from the One mark after a full POST. Otherwise (v6, the unbranded iOS path, quick or off) the
  panel is put back as before.
- **Quick POST** draws nothing over S-Boot's splash unless Volume Up or log level 2-3 asks for its
  lines; a red check shows the same failure screen (the panel is saved and put back around it).
- Log page slot 245: `RAM ms << 32 | flags` (1 log by Volume Up, 2 log from the start, 4 failure
  screen, 8 logo kept for the menu, 16 S-Boot's splash already was the logo, 32 Volume Up held at the
  start, 64 skipped).

| Check | How (full) | Bound | Fail (red) | Warn (gold) |
|---|---|---|---|---|
| RAM | address and ~address over windows A, B, C (2.3); each window written whole, then read whole (aliasing shows), then 0x55/0xAA over A while time is left | **6 s budget, reads included** (below) | any bad word (**fatal**) | Volume Down cut it short |
| TIMER | ticks between two PMIC RTC second edges | 2 × 600 reads | counter stopped (**fatal**) | > 3 % off 26 MHz; RTC unread |
| CPU | MIDR (A55), A55 cluster MHz from PLL_CPUCL0 / CMU_TOP; M3 MHz only if PMU 0x14062044 says powered | register reads | — | PLL unlocked |
| TEMP | every TMU probe: big cluster max, little cluster, GPU/ISP | reads | ≥ 90 °C | ≥ 70 °C; TMU off |
| PMIC | ACPM channel + S2MPS18 ID | acpm 100 ms waits | no channel / no answer | > 250 ms |
| BATTERY | MAX77705 via `batt.c` (read only): %, VCELL, current, cable/charging | batt.c deadlines | no answer / unread | < 10 %, VCELL outside 3.3–4.45 V |
| DISPLAY | DECON0 trigger (0x16030070), IDMA ch0 base and size (0x16071040/10), and **frame-done**: the pending bit 13 of 0x1603004c is cleared (write 1) and must come back within 60 ms; it is set by the panel's TE through the hardware trigger | 60 ms | — | no frame seen; scanout not at 0xCC000000 |
| TOUCH | `hw_tp_start` (S6SY761) | menu.c bounds | no answer | > 600 ms |
| USB | PMU 0x1406072c bit 0 (PHY isolation) and the charger's cable bit. The PHY's own registers are **not** read: their clocks may be off | reads | — | — |
| UFS | `ufs_up` | ufs.c (2 s/command) | host/device | > 2 s |
| GPT | `gpt_load`, USERDATA present, **primary header CRC + entry array CRC + the backup header** at its alternate LBA | a few 4 KiB reads | unreadable, no USERDATA, primary CRC bad | backup bad |
| BOOT | BOOT block 0: `ANDROID!`, page 2048, kernel `ARMd` at 2048+56, `DTBH` at the DT offset | 2 reads | no header / not arm64 | no DTBH |
| STORE | v2 header + directory CRCs (`store_open`), entry count, **`crashlog` entry present** | a few reads | none / bad CRC | empty; no crash log |
| CATALOG | the store's catalog CRC + parse; **visible entries whose files are missing** | directory only | bad CRC (built-in used) | none; entries not ready |
| SETTINGS | the settings record: CRC/version, levels | 1 read | — | bad CRC, no store |
| MICROSD | card detect GPA1-5; the card store's header **only where the loader may touch the card** (settings flag "card store at boot" or EngineerMode allowed it) | sd.c | — | card store missing |

Only RAM and TIMER are fatal (Starbite Recovery comes up instead of the menu), as before.

**The RAM budget (fixed 2026-11-07).** The first phone run showed the old progress bar full seconds
before the RAM test ended: the budget was only checked before each 4 MiB write, so whatever window C
had written by then was read back after the budget, and non-cacheable reads are much slower than
writes. Now a chunk is written only while there is time left to read back everything written in that
pass, at the measured speeds (the last chunk's write and read times; 3 x the write time until a read
was timed), with 1/8 to spare for the keys and the logo. The 0x55/0xAA passes run chunk by chunk
while a write + read still fits. So RAM ends at about 6 s, and slot 245 records its real time.

### 2.3 RAM windows

| Window | Range | Why it is free during POST |
|---|---|---|
| A | 0x08C000000 + 64 MiB | STAGE, below 0x90000000 where S-Boot's firmware log windows begin; the catalog buffer there is parsed again after POST |
| B | 0x092000000 + 128 MiB | the XNU window, below 0x9A000000 (POST's panel copy, recovery 0x9C000000, quick POST rows 0x9E000000, the Bixby hint 0x9E800000, the crash buffers 0x9F000000) |
| C | 0x880000000 + 1 GiB | DRAM bank 2 (S-Boot's memory node; no reserved-memory in any DT here), as far as the budget allows |

**Never**: 0xA0000000 (+0x32000, S-Boot's ECT, which the Android kernel reads), anything below
0x8C000000 (the loader, menu assets 0x89000000, S-Boot's FDT 0x8A000000), 0x90000000..0x92000000
(firmware log windows, fastboot's DMA), 0x9A000000..0xA0000000, the ramoops area and
0x80000000 (crash evidence). Quick POST keeps its 1 MiB at 0x8C000000.

### 2.4 Time on a healthy phone (estimate)

RAM 6 s (budget, now including its reads) + timer about 1.5 s (waits for two RTC edges) + touch 0.2 s
+ display 0.06 s + storage, battery, rest about 0.2 s = **about 8 to 8.5 s**, plus the 400 ms Bixby
window. No closing pause behind the logo (the log view pauses 2 s). `POST_RAM_MS` in post.c sets the
RAM share.

### 2.5 Results

- `post_last()` for the menus. The recovery menu shows the failures.
- Log page 0x91fff000: slot 200 `"POST" << 32 | mode << 24 | n << 16 | fails << 8 | warns`,
  slot 201 total µs, per check `state << 56 | value << 32 | µs`: **slots 202..212 for checks 0..10**
  (as before, which SBMAINT's `sbm logs` decodes) and **240..244 for the new checks 11..15** (202 + 14
  would reach recovery's slot 216). Ids in post.h.
- pmsg ring: `STARBITE: POST full 8712ms fail 0 warn 1` and one line per failure or warning;
  `STARBITE: POST: Volume Up: the live log`, `STARBITE: POST: failure screen` when they happen.
- Slot 245: how it was shown and the RAM test's real time (2.2).

## 3. Log levels (request b)

One global setting, the settings flags bits 8-9 (`SET_LOG_*`): **0 quiet (the default: debug output
off)**, 1 normal, 2 verbose, 3 debug. To change it: Starbite Recovery → STARTUP SETTINGS → LOG LEVEL,
`sbstore2.py settings OUT --from BLOCK --log verbose`, or Engineer OS v2's settings editor.

### 3.1 The loader

- **Level 0**: the loader's own status lines during a start ("LOADING LINUX", "KERNEL AT ...",
  "MICROSD POWER ON") are hidden (`star_fb_mute`). The previous state is restored if the start
  fails. Menu v7 already hid them while its launch card is shown.
- **Levels 1 and 2**: as before.
- **Level 3**: the status lines are shown even under menu v7.
- POST and the crash screen are not debug text: they show at every level.

### 3.2 The operating systems: catalog fragments

Each catalog entry has four command line fragments, one per level. The loader appends the current
level's fragment to the entry's cmdline. They come from `tools/store/catalog.toml`: `log = [...]`
on an entry, or `[defaults.METHOD]` for every entry of a method that sets none.
`sbstore2.py catalog/build` packs them into the catalog's extension table `SBCATEX1`
(catalog.h). Older loaders ignore that table, and older catalogs have none: then nothing is
appended, exactly as today.

| Method | 0 quiet | 1 normal | 2 verbose | 3 debug |
|---|---|---|---|---|
| linux (`[defaults.linux]`) | `quiet loglevel=3 vt.global_cursor_default=0 systemd.show_status=auto rd.udev.log_level=3 udev.log_level=3` | `loglevel=4` (the DTB's own) | `loglevel=7 s9p_decon.geom_log=1 systemd.log_level=info` | `loglevel=8 ignore_loglevel keep_bootcon printk.devkmsg=on s9p_decon.geom_log=1 systemd.log_level=debug` |
| android | — | — | `loglevel=7` | `loglevel=8 ignore_loglevel` |
| xnu | — | — | `!a5fb_noconsole !sweetquiet` | `!a5fb_noconsole !sweetquiet -enable_kprintf_spam` |
| uefi, bare | no command line | | | |

- **Linux**: later parameters win (`quiet` and `loglevel=` are early parameters processed in order),
  so the fragment overrides the DTB's `loglevel=4` and the entries' `s9p_decon.geom_log=0`. The
  initrd's own `echo` lines (tools/linux/init) still print at level 0. To quiet them, the initrd
  could honour `quiet`. That file belongs to the Linux owner.
- **Android**: Samsung's kernel prints nothing on the panel at any level (`console=ram`). At levels 0
  and 1 the fragment is empty, so the Android start is **byte for byte the old one**.
- **XNU**: appending cannot undo the payload's `-v` (a flag cannot be switched off). A fragment
  token `!name` therefore first **removes** every `name` and `name=...` token from the payload's
  boot-args (`sb_line_edit`). (Correction found with 0399: for `name=value`, pexpert's
  `PE_parse_boot_argn` keeps the **last** occurrence, since it scans on after a match;
  pexpert/gen/bootargs.c. Only `-flags` stop at the first. So an appended `name=value` does win
  over the payload's; `!name` stays the clean way.) The daily payloads are already
  quiet on the panel (`a5fb_noconsole=1 sweetquiet=1`), so levels 0 and 1 change nothing. Levels 2
  and 3 bring the text console back.
- `linux-bringup` sets its own `log = ["", "", "", ""]`: it is a debugging entry.

## 4. Crash and hang detection (request c)

### 4.1 Where it runs in the boot

```
xnu_boot.S → MMU → star_fb_enable
── Bixby window (unchanged: key GPIO, timer, fbtext) ──
crash_snapshot      copy 0xFED10000+32 KiB and 0x80000000+4 KiB to RAM; read PMU RST_STAT,
                    INFORM2/3, WDT and PMU-mask registers; parse S-Boot's bootargs
                    (sec_debug.reset_reason/rst_stat/pwronsrc/pwroffsrc).
                    Reads and a RAM copy only, BEFORE the first pmsg line (which would reset a
                    full Linux console ring).
latch dump, rescue, recovery (if Bixby), splash, EL checks
settings_load       (UFS) → the POST level
POST                (full / quick / off)
cat_load
crash_after         judge the last launch, save a report, crash screen, then menu / recovery / retry
fastboot hook, boot menu
boot_entry          the entry's copy gets the log fragment + watchdog arguments
star_prepare_xnu → launch_go:  launch record (crash log, UFS write) → wdt_arm (last thing)
xnu_boot.S → OS
```

A start from Starbite Recovery or fastboot that comes before `crash_after` judges a pending launch
first, without the screen. Rescue mode writes no launch record: it must not need UFS.

### 4.2 Evidence

| Source | Where | Meaning |
|---|---|---|
| PMU RST_STAT | 0x14060404 | why the SoC reset. Bits (sec_debug_reset_reason.c, Exynos 9810): 16 PINRESET, 23 CLUSTER1_WDTRESET, 24 CLUSTER0_WDTRESET, 28 WRESET, 29 SWRESET; 19/20/22/25/26 other watchdogs; 0..15 debug/warm requests |
| PMU INFORM3 | 0x1406080c | the reboot reason the OS wrote: 0x12345670 normal, ...74 recovery, ...71 download; Samsung's panic writes 0xC8 |
| S-Boot's verdict | its `/chosen/bootargs` | `sec_debug.reset_reason=N` (1 SPON, 2 WPON watchdog, 3 DPON, 4 KPON kernel panic, 5 MPON manual reset, 6 PPON, 7 RPON reboot, 8 BPON, 9 NPON power on, 10 TPON), `sec_debug.rst_stat`, `pwronsrc` (S2MPS18 PWRONSRC: bit 3 MRST, bit 6 SMPL), `pwroffsrc` |
| Linux ramoops dmesg | 0xFED10000, zone 0 (16 KiB, "DBGC" ring) | `====<sec>.<usec>-D` + `Panic#1 Part1` / `Oops#1` + the kernel log at the moment of the panic (not compressed: `CONFIG_PSTORE_COMPRESS` is off) |
| Linux console | 0xFED14000, zone 1 | the running kernel log, so a hang leaves its last lines |
| Samsung sec_debug | 0x80000000 (4 KiB, reserved `sec_debug_magic`) | magic 0x66262564 + the panic string at +4 |
| Android console | 0xFED10000 zone 0 (exynos-snapshot's log_pstore) | the vendor kernel's console tail |
| XNU | pmsg ring 0xFED14000 | `STARBITE: XNU panic (sweetreboot), panic log head:` + (0399) the crash marker line + the panic log head + console tail, or `STARBITE: sweetreboot timer (no panic), console tail:` (series-12377/0309, 0349, 0399; pending-mac13/0312). Before 0399 the 12377 timer wrote the panic header too (section 5.4) |
| Windows (woa) | pmsg ring + RWD1 0xFED13D80 | the UEFI's trace; shown as the log tail |
| The launch record | the crash log | which entry was started, when, watchdog, flags |

Evidence is used only for the method that was launched. Each piece's CRC is remembered in the
crash log header (`seen_*`), so a ramoops record that survives several boots is reported once.

### 4.3 The decision (`sb_classify`, host-tested table)

In order:

1. No launch pending: a watchdog reset is "WATCHDOG, NO OS" (warn); anything else is nothing.
2. A fresh Linux `Panic#` record, an Android sec_debug panic, or an XNU panic dump: **KERNEL PANIC** (crash).
3. RST_STAT has a watchdog bit, or S-Boot says WPON: **HANG (WATCHDOG)** (crash).
4. XNU's sweetreboot timer dump: **HANG (OS TIMER)** (crash).
5. S-Boot KPON, or INFORM3 0xC8: **KERNEL PANIC** (crash).
6. S-Boot SPON / DPON / PPON: **FAULT** (crash).
7. S-Boot MPON, RST_STAT WRESET, or PWRONSRC MRST without SWRESET: **FORCED RESTART** (warn). This is
   usually a hang the user ended with Volume Down + Power.
8. A fresh Linux `Oops#` (or another record) and the system went on: **KERNEL OOPS** (warn).
9. S-Boot TPON / BPON: **FAULT** (warn).
10. PINRESET or NPON: **POWER ON** (info). If PWRONSRC has SMPL (power lost), warn.
11. SWRESET or RPON: **CLEAN RESTART** (info).
12. Anything else: **UNEXPLAINED RESET** (warn).

Info is logged only. Warn and crash are saved as a report, show the crash screen, and **stop the
menus' auto-start countdown for this boot**, so a crashing default entry does not loop.

### 4.4 The crash screen: a scrolling report (2026-11-07)

The first real panic on the phone (`echo c > /proc/sysrq-trigger`) was detected and shown; the
user: "its nowhere near detailed enough ... it must stay similar text style but better, and so you can
actually see more info". The screen is now a report in the same 5x7 pixel font (with lower case),
small: `star_fb_mini`, 18 x 36 px cells, **76 columns, 78 rows a page** plus a footer.

**What it shows**, top to bottom (`doc_build` in crash.c):

| Part | Content |
|---|---|
| Header | `STARBITE CRASH REPORT #n` and the kind ([CRASH] red / [WARN] gold); OS name (key, method); **KERNEL** (Linux: the release from the `CPU: ... Not tainted <release> #n` line or `Linux version`; XNU: the `root:xnu-...` of `Kernel version:`); **CRASHED** (the record's wall time, or when the loader found it) and **UPTIME** at the panic (Linux: the panic line's timestamp; XNU: the crash marker's `uptime N s`); **STARTED** and **TASK** (`CPU 2 PID 1 init`); **REASON**; **RESET** RST_STAT with its bit names, INFORM2, INFORM3 with its name; **S-BOOT** reset code and name, rst_stat, pwronsrc, pwroffsrc; **WATCHDOG** at the launch and WDT1's registers; **SOURCE** (which ring, lines, bytes); **STORE** `report #n (slot k)` = the `--- report #n` of `sbstore2.py crashlog live --level 3` |
| PANIC MESSAGE | every message line: `Unable to handle kernel ...`, the `Mem abort info:` / `Data abort info:` blocks, `Internal error: Oops: ...`, `BUG:`, `Kernel panic - not syncing: ...`; XNU's `panic(cpu ...)` line |
| CONTEXT | `CPU: ... PID: ... Comm: ...`, `Hardware name:`, `Modules linked in:`, `Kernel Offset:`; XNU's marker lines, `Debugger message`, `OS version`, `Kernel version`, `Panicked task` (details and up) |
| REGISTERS | Linux `pstate:`, `pc :`, `lr :`, `sp :`, `x29: ... x0:`, `Code:`; XNU `x0: ... far:` and `CORE n: PC=...` (details and up; "no register dump" for a plain `panic()` such as sysrq's) |
| CALL TRACE | Linux `Call trace:` and its frames; XNU `Panicked thread: ..., backtrace:` and its `lr: ... fp: ...` lines (summary: the first 12 frames) |
| RESET AND WATCHDOG REGISTERS | all 16 saved registers with names (everything) |
| LOG | summary: the last 30 lines; details: the last 150; **everything: all of it** |
| OTHER RING | Linux: the other ramoops zone (the console when the dmesg record was the source), everything only |

- The sections leave out the `<N>` syslog prefix and the `[ time]` stamp (so a register line fits one
  row); the LOG keeps the stamps. Long lines wrap at a space in the row's second half.
- **Right after the crash the report shows everything the loader captured this boot** (the ramoops
  zones as text, up to 16 KiB each, `live_*` in crash.c). A report opened later from Starbite
  Recovery → CRASH REPORTS shows what the store keeps (below).
- **Levels** stay presets: 1 SUMMARY, 2 DETAILS, 3 EVERYTHING (the setting; Recovery's list opens at 3).
- **Keys**: Volume Up / Down scroll a line (held: repeats after 300 ms, after 1.5 s 8 lines at a
  time); Bixby pages on, stopping at the next section's start (at the end: back to the top); Bixby held
  0.7 s: the next detail level; Power continues.
- **Footer** (one line): `VOL SCROLL  BIXBY PAGE (HOLD: DETAIL 2/3)  POWER CONTINUE  1-78/233  45s`.
- **Timeout**: `crash_timeout` (default 60 s, counted from the last key), capped at 10 minutes.
- RAM: the composed lines at 0x9F030000 (64 KiB), the rows at 0x9F040000 (6144 x 16 bytes), the
  lines' kinds at 0x9F060000 (16 KiB); all below S-Boot's ECT (`_Static_assert`).
- The panel is saved and put back around the screen; then, by `crash_action`: the boot menu (default),
  Starbite Recovery, or **retry once** (`CLF_RETRY`, a second crash goes to the menu).

**The saved report's text** (`cl_excerpt`, host-tested): the store keeps 3824 bytes per report. It
holds every message, context, register and trace line, then as many of the last lines as fit, in
their original order, `<N>` prefixes dropped, each run of skipped lines as one `[...]` line. So a
report browsed later (or read on the Mac) still has the registers and the call trace.

**The parser** (`sb_crash_parse` in sbfmt.c) classifies each line (`CT_MSG`, `CT_INFO`, `CT_REGS`,
`CT_TRACE`, `CT_LOG`) and fills the header's version, task, message and uptime. Host-tested on a Linux
oops that panics (lkdtm EXCEPTION, 121 lines), the sysrq panic the phone ran, and an XNU 0399 data
abort dump, each also through a ramoops / pmsg ring as crash.c reads it.

### 4.5 Settings (Starbite Recovery → STARTUP SETTINGS, or `sbstore2.py settings`)

| Row | Values | Where |
|---|---|---|
| POST AT BOOT | FULL, QUICK, OFF | flags bits 4-5 |
| LOG LEVEL | 0 QUIET, 1 NORMAL, 2 VERBOSE, 3 DEBUG | flags bits 8-9 |
| CRASH DETECTION | ON, SAVE ONLY (no screen), OFF (no launch record, no INFORM3 patch) | byte 104 |
| CRASH SCREEN SHOWS | 1 SUMMARY, 2 DETAILS, 3 EVERYTHING (4.4) | 105 |
| AFTER A CRASH | BOOT MENU, RECOVERY, RETRY ONCE | 106 |
| CRASH SCREEN WAITS | 60, 120, 300, 600, 30 s | 107 (seconds / 10, 0 = 60) |
| WATCHDOG | PER ENTRY, OFF, 30 S, 60 S, 82 S | 108 mode, 110 u16 seconds |

The page reads the record when it opens and writes it only on SAVE.

### 4.6 The crash log: store entry `crashlog`

A UFS store entry of 16 blocks (64 KiB). Its directory flags are 1 (`SF_VOLATILE`): the loader
writes it in place, so its CRC is not checked by store.c, sbstore2.py `read_store`/`list`, and it
is left out of the version 1 view. `sbstore2.py build --medium ufs` adds it, and `--import` keeps
the phone's reports. Card stores get none. The loader writes only this entry's blocks
(`store_crashlog_write` refuses any other) and each write is read back.

- **Block 0, the header** (CRC32 of bytes 16..1023 at 12): `SBCRASH1`, version 1, slots (15), slot
  size (4096), next slot, reports written, launches judged, launch sequence, launch state (1 =
  running), method, watchdog armed, watchdog s, key[24], name[32], launch time (UTC s), log level,
  launch flags (1 Linux INFORM3 patched, 2 fastboot, 4 rescue, 8 retry), and the seen CRCs of the
  dmesg record, console, sec_debug string, and XNU dump.
- **Blocks 1..15, the reports** (CRC32 of 16..4095 at 12): `SBCRREP1`, version 1, number, kind,
  severity, method, source, detected / launched / event times, key, name, 16 registers, reason[96],
  then up to 3824 bytes of the last log lines (printable ASCII, `\n`).
- **Writes per OS start**: one block before the handoff (the launch record), and at the next loader
  start one header block, plus one report block if there was a crash. Every write is bounded by
  ufs.c. A failed write is logged and ignored.

**Reading it on the Mac**:

```sh
# the phone in the loader's fastboot (hold Volume Up at boot, or `fastboot reboot bootloader`)
python3 -I tools/store/sbstore2.py crashlog live --level 3   # runs: fastboot oem log crashraw + get_staged
fastboot oem log crash && fastboot get_staged crash.txt     # the same as text, from the loader itself
# or from a pulled store image / the raw entry
python3 -I tools/store/sbstore2.py crashlog build/store/ufs-live.img --level 2
```

Log page slots 220..223: `"CRSK" << 32 | kind << 24 | severity << 16 | evidence`,
`RST_STAT << 32 | INFORM3`, `S-Boot rr << 32 | launch seq`, reports written. A pmsg line:
`STARBITE: crash: last launch linux-pmos (linux): KERNEL PANIC [crash] rst 0x20000000 inform3
0x12345670 sboot_rr 7`. The launch line: `STARBITE: launch linux-pmos #12 watchdog 60 s`.

### 4.7 Making each OS come back to the loader

A crash can only be shown if the reset lands in the loader, not in RECOVERY or upload mode:

- **Linux**: the port's DTB writes INFORM3 = **recovery** early (`/clock-gates s9p,set-registers`),
  so today a panic or watchdog reset lands in RECOVERY. With `crash_return = true` (the
  `[defaults.linux]` value) and crash detection on, the loader **patches that pair in its copy of
  the DTB to 0x12345670 (normal)** (`fdt_patch_pairs`; log slot 20 = pairs patched). `panic=10` in the
  DTB's bootargs makes a panic reset after 10 s. A clean `reboot` still writes INFORM3 through
  `syscon-reboot-mode`.
  **Restart path (2026-11-06, kernel step 10, LINUX-REBUILD-PLAN 7)**: until then the kernel's only
  restart handler was the cluster-1 watchdog (PSCI is v0.1 here, no SYSTEM_RESET), and `reboot -f`
  never reset (the watchdog's reset was masked again by its driver's shutdown) while a panic sat on
  screen for about two minutes. The kernel now restarts the way Samsung's does: INFORM2 =
  0x12345678, then PMU SWRESET, leaving INFORM3 as written (reboot-mode's value for `reboot`, the
  boot value for a panic). And the panic shows on the panel at once (kernel step 6g): a Starbite
  banner over the kernel log's tail, for the 10 s before the reset; the loader's crash screen
  follows as before.
- **Android (Samsung 4.9)**: S-Boot gives `androidboot.debug_level=0x4f4c` (LOW): panics and
  watchdog resets reboot normally (no upload mode) and S-Boot passes KPON/WPON. Nothing to do.
- **XNU**: before 0399, the panic hook (`starlte_panic_to_recovery`, bsd/dev/herolte_ufs.c on 12377)
  ran only with `sweetreboot=N` and wrote INFORM3 = 0x12345674, so panics landed in RECOVERY.
  **Done in xnu/series-12377/0399 (section 5.4)**: with `sbcrash=loader` the hook runs whatever
  `sweetreboot` says, writes the dump the loader reads plus a crash marker line, then INFORM3 =
  0x12345670 + SWRESET. XNU also writes INFORM3 = normal at boot, as the Linux DTB patch does. The
  loader has no `crash_return` path for XNU, so `sbcrash=loader` comes from the catalog's log
  fragments (tools/store/catalog-xnu-health.toml).
- **Windows**: the UFS driver sets INFORM3 for its own restarts; its bugchecks restart normally.

## 5. The hardware watchdog

### 5.1 Facts and sources

| Fact | Value | Source |
|---|---|---|
| Watchdogs | cluster 0 (A55) 0x10050000 `samsung,exynos8-wdt`, timeout-sec 30, index 0; cluster 1 (M3) 0x10060000 `samsung,exynos7-wdt`, timeout-sec 20, index 1, `use_multistage_wdt` | stock DT `ref/vendor-snips/exynos9810.dtsi` lines 4177-4198 |
| Clock | `rate_watchdog = OSCCLK` (26 MHz), gate `UMUX_CLKCMU_PERIS_BUS` | same; mainline port DT `wdt_clk` 26 MHz |
| Registers | WTCON +0 (prescaler [15:8], ENABLE 5, DIV [4:3] 16/32/64/128, INTEN 2, RSTEN 0), WTDAT +4, WTCNT +8 (16-bit), WTCLRINT +0xC | mainline `drivers/watchdog/s3c2410_wdt.c` (the VM's tree, /mnt/build/starbite/src/mainline) |
| PMU | RST_STAT 0x14060404, AUTOMATIC_WDT_RESET_DISABLE 0x14060408, MASK_WDT_RESET_REQUEST 0x1406040C; **bit 23 = cluster 1, bit 24 = cluster 0**; a set bit blocks the reset | s3c2410_wdt.c `drv_data_exynos7` (mask_bit 23, rst_stat_bit 23, EXYNOS5_*_OFFSET); RST_STAT names in sec_debug_reset_reason.c; NOTES 2026-10-26 (the woa bootshim unmasks bit 24 for 0x10050000) |
| S-Boot's state | both watchdogs counting, their resets masked (bit 23 set) | port DT comments (`s9p,clear-registers = <0x1406040c 0x800000>`); EngineerMode's register page |
| Maximum timeout | prescaler 255, divider 128: 26 MHz / 32768 = 793.457 Hz; 65535 counts = **82.6 s** | `sb_wdt_count`, `s3c2410wdt_max_timeout` (same formula) |
| WTCON used | 0xFF39 (prescaler 255, DIV128, enable, reset) | the same value the woa bootshim and the port DT use |

### 5.2 Arming (`wdt_arm`, the last thing before the OS)

The sequence:

1. WTCON = 0 (stop it: S-Boot left it counting).
2. WTDAT = WTCNT = counts.
3. WTCLRINT = 0.
4. WTCON = 0xFF39.
5. Clear bit 23 in 0x14060408, then in 0x1406040C.

The timeout per start:

- `wdt_mode` OFF: none.
- FIXED: `wdt_s` for every entry whose catalog watchdog is not 0.
- PER ENTRY (default): the entry's `wdt`.
- In every mode, only if `wdt_feed` fits the method. The value is clamped to 10..82 s.

Only cluster 1's watchdog is used. Cluster 0's stays as S-Boot left it (masked). Log slot 19:
`armed ms << 40 | seconds << 32 | log level << 8 | INFORM3 patched`.

### 5.3 Who feeds it

| OS | How | Catalog |
|---|---|---|
| **Linux** (mainline 7.1) | Mainline's driver **stops a running watchdog at probe unless `tmr_atboot=1`** (s3c2410_wdt.c probe; the DTB passes `tmr_atboot=0`). So the loader appends `s3c2410_wdt.tmr_atboot=1 s3c2410_wdt.tmr_margin=T`. The driver restarts it at probe with T and sets WDOG_HW_RUNNING, and the watchdog core pings it from the kernel (`CONFIG_WATCHDOG_HANDLE_BOOT_ENABLED=y`, already in starbite.config, as is `CONFIG_S3C2410_WATCHDOG=y`) until user space opens /dev/watchdog. This catches kernel hangs. With `wdt_feed = "userspace"` the loader also appends `sb.wdt=T`: the initrd (tools/linux/init) hands the watchdog to systemd (`RuntimeWatchdogSec=T`) or busybox `watchdog`, which catches user-space hangs too. Optional: `watchdog.open_timeout=N` reboots if nothing opens it within N s | `[defaults.linux]`: wdt 60, feed kernel |
| **Android** (Samsung 4.9) | Its own exynos watchdog driver takes both watchdogs over at probe. S-Boot passes `sec_watchdog.sec_pet=5` (the kernel pets every 5 s) and `softdog.soft_margin=100 softdog.soft_panic=1` (user-space hangs panic → KPON). The loader arms nothing by default. `wdt = N` with `wdt_feed = "own"` would cover only the time until the driver probes | `[defaults.android]`: wdt 0, feed own |
| **XNU** | `wdt=-1 -wdt_no_pmu` only concern Apple's own watchdog. **Done in xnu/series-12377/0399 (section 5.4)**: `sbwdt=N` (the loader appends it with `wdt_feed = "xnu"`): XNU maps 0x10060000, checks WTCON/WTDAT, and a HIGH priority `thread_call` writes WTCNT (+8) = WTDAT (+4) every N/3 s, never after a panic starts. Only payloads with the 0399 kernel may get `wdt` > 0 | `[defaults.xnu]`: wdt 0; per entry 60 once 0399 payloads passed the phone tests |
| **Windows / UEFI** | Off. The woa firmware and Exynos9810Ufs.sys own cluster 0's watchdog through the RWD1 handshake (NOTES 2026-10-26); a second owner would fight it | none |
| **Bare** (Engineer OS) | Off unless libsb pets it | none |

### 5.4 XNU's side: xnu/series-12377/0399 (2026-11-06)

**Status: implemented and built; nothing has run on the phone.**

- `herolte_ufs.c` compiles with the kernel build's own compiler command (from its `.o.json`), with no new
  warnings under `-Weverything`.
- The patch applies with `git apply` on the series state (build/dxb-12377) and gives exactly the
  compiled sources.
- The kernel was linked by replaying the logged link (`build/xnu-12377.log`). With the old objects, the
  replay gives today's kernel byte for byte, except the `__CTF` dtrace section, which ctfmerge never
  makes the same twice. With 0399, it is 21422296 bytes.
- Host test: `sh tools/xnu-health-test/run.sh`, 911 checks (Mac clang, ASan + UBSan).

**Which kernels have it**:

| Kernel | Started by (catalog) | 0399 |
|---|---|---|
| xnu-12377 (`xnu/series-12377`) | every `method = "xnu"` entry: mac13 (Ventura, `mac13-v33.xnu`), ios26 (`ios26-sd.xnu`), darwin, sdprobe, ios26sdt | **yes** |
| iOS 16.5 | `ios16` is `method = "uefi"`: an earlier port's own kernel, not built here | no (not ours) |
| xnu-8796 (`xnu/patches/03*`, `pending-mac13/`) | no entry; pending-mac13 has never been compiled | no. To port: the same code into pending-mac13/0312's `bsd/dev/starlte_pmsg.c`, which has the same structure |
| xnu-11417, xnu-7195 | no entry | no |

**Boot-args** (`starlte_health.h` holds the constants and the pure math):

| Boot-arg | What XNU does |
|---|---|
| `sbwdt=N` | At STARTUP(KMEM) it maps cluster 1's watchdog (0x10060000) and the PMU and reads WTCON/WTDAT. If the watchdog can reset the SoC (ENABLE, RSTEN, count > 0), it feeds it once: WTCNT = WTDAT. From STARTUP(EARLY_BOOT), a `THREAD_CALL_PRIORITY_HIGH` thread call feeds it every min(N, hardware timeout) / 3, between 1 s and 27.5 s. With 60 s that is 19999 ms. A watchdog that is not armed is only read, never written. |
| no `sbwdt=` at all | Fed anyway, for its own timeout, but only if the watchdog is armed **with its reset unmasked in both PMU words** (bit 23 of 0x14060408 and of 0x1406040C clear). Only the loader's `wdt_arm` leaves it like that; S-Boot masks it. This covers the loader dropping the token (hazard 2). |
| `sbwdt=0` | Never fed. |
| `sbcrash=loader` | At boot: INFORM3 = 0x12345670, so a watchdog reset or a forced restart comes back to the loader. A panic: the dump (below), INFORM2 = 0x12345678, INFORM3 = 0x12345670, SWRESET. S-Boot starts the loader, whose crash screen shows KERNEL PANIC. This works with or without `sweetreboot`. |
| `sbcrash=recovery` | A panic: the dump, then INFORM3 = recovery (TWRP), as `sweetreboot` did. |
| `sweetreboot=N` (unchanged) | Panics go to TWRP unless `sbcrash=loader`. The N-second timer still resets into TWRP (the unattended cycle's way back). Its dump now has its own header, `STARBITE: sweetreboot timer (no panic), console tail:`, as the 8796 port already had. The loader reads it as HANG (OS TIMER), and `tools/starlte-cycle.sh` no longer reports it as a panic. |
| neither, `sbwdt` armed | A panic: the dump, then the spin. The panic stays on screen until the unfed watchdog resets the phone. The loader sees the dump: KERNEL PANIC. |
| none of these | No hook, as before 0399: a panic spins for ever. |
| `sbtestpanic=S` | Test: `panic("STARBITE: sbtestpanic=S: test panic ...")` S s after EARLY_BOOT. |
| `sbtesthang=S` | Test: feeding stops S s after EARLY_BOOT (a simulated hang). |

**Feeding stops for good**:

- When a panic starts: the panic hook sets the stop flag, and every feed checks `panic_active()`.
- While XNU is in the debugger or panic path, the other CPUs are halted by IPI and the trapping one
  runs with interrupts off, so no feed can run.

So a whole-kernel hang (scheduler dead, timers dead, all CPUs stuck) or a panic path that never
reaches the reset is caught by the hardware. A single stuck CPU is not: the other seven keep feeding.
XNU's own timeouts (IPI, TLB) turn that case into a panic, which then takes this path.

**The dump** in the pmsg ring (0xFED14000, `"DBGC"` sig/start/size; `sb_ring_text` reads it):

```
STARBITE: XNU panic (sweetreboot), panic log head:      the loader's needle, kept byte for byte
STARBITE: xnu crash v1: return loader, uptime 1234 s, watchdog 60 s fed 41 times
panic(cpu 2 caller 0x...): <the panic string>           the loader's reason line (first "panic(")
...the panic log head (at most 11 KiB)...
--- console before the panic:
...the console tail, up to 0x3F00 bytes in all
```

- Data never goes past 0xFED14000 + 12 + 0x3F00 = 0xFED17F0C, so the boot-next mailbox at 0xFED17FE0 is
  untouched (`_Static_assert`, and the host test checks it).
- The marker line never contains `panic(`.
- If no panic log was written yet (a very early panic), the line is `panic(unformatted): <format string>`.
- The hook runs from `panic_spin_forever`. That needs `debug=` with DB_NMI (0x4); without it the panic
  path ends in `halt_all_cpus` and only the watchdog resets the phone. Every daily payload has
  `debug=0xc`.

**Console lines** (shown at log level 2 or 3, or in the persistent log):

```
STARBITE HEALTH: sbwdt=60: cluster 1 watchdog WTCON 0xff39 WTDAT 47607 (59999 ms) WTCNT ... at KMEM, PMU disable 0x... mask 0x...; fed every 19999 ms until a panic
STARBITE HEALTH: sbcrash=loader: INFORM3 0x... -> 0x12345670; panics and resets come back to the loader
HEROLTE LOG: panic hook armed (PMU ..., pmsg ...): a panic resets to the loader (sbcrash=loader)
STARBITE HEALTH: watchdog feeder running (thread call, every 19999 ms)
```

**Hazards and decisions**:

1. **An old kernel with `wdt` > 0 resets after N s**: the loader arms, nothing feeds. The loader
   cannot tell which kernel a payload holds. So `wdt` is set **per entry**, only where the entry's
   file is a 0399 payload, never in `[defaults.xnu]` while old payloads exist. Today's mac13-v33,
   ios26-sd, ios26-sdt, darwin and sdprobe are all old kernels.
2. **Full boot-args**:
   - The payload holds 607 bytes. `sb_line_edit` stops appending at the first token that does not fit,
     but the loader still arms the watchdog.
   - Headroom today: mac13-v33 517 bytes (90 left), ios26-sd 538 (69 left), ios26-sdt 593 (14 left:
     neither `sbcrash=loader` nor `sbwdt=N` fits).
   - 0399's fallback above feeds a loader-armed watchdog anyway.
   - **Done 2026-11-07**: `boot_xnu` checks the finished boot-args (`sb_wdt_args_ok`, the last
     `sbwdt=` must be the armed value) and does not arm when the token was dropped; Linux the same for
     `s3c2410_wdt.tmr_atboot=1` + `tmr_margin=T` in the entry's cmdline (without them mainline's driver
     stops or re-times the watchdog). pmsg `STARBITE: watchdog NOT armed for KEY: sbwdt= did not fit
     the command line (60 s asked)`, log slot 19 bit 16, the launch record says no watchdog. The order
     of the appended tokens is unchanged.
3. **The loader has no `crash_return` for XNU.** `sbcrash=loader` travels in the log fragments, so it
   also applies when the settings say CRASH DETECTION OFF. Harmless: the loader then just shows the
   menu.
   - **For the loader owner**: `launch_prepare` could append `sbcrash=loader` for `M_XNU` entries with
     `CE_LINUX_INFORM3` (crash_return) and crash detection on, the way Linux's INFORM3 patch is gated.
4. **Unattended TWRP cycles**:
   - With `sbcrash=loader` from `[defaults.xnu]`, a panic in a cycle stops at the loader's crash screen,
     then at the menu with no countdown.
   - A cycle that must come back to TWRP boots an entry whose own `log` fragments end in
     `sbcrash=recovery`. That is the last occurrence, so it wins.
   - The `sweetreboot` timer always goes to TWRP.
5. **System sleep**: this port has no system sleep. If XNU ever stopped every CPU's timers for a
   suspend, the watchdog would reset it. The soak test (X5) covers idle and display sleep.
6. **Rebuild**: the series hash changed, so `FRESH=1 tools/build-xnu-12377.sh`. It rebuilds
   build/dxb-12377 (about 1.3 GB, as now).

**Artifacts** (in this worktree's `out/`, git-ignored; not flashed, not in any store):

| File | sha256 | What |
|---|---|---|
| `kernel.development.starlte-12377-health` | 24ddb06c09ff509a... | today's 12377 kernel + 0399, linked as above |
| `xnu-mac13-health.xnu` | 9729ad1cc5d2610a... | mac13-v33's boot-args (517 bytes) + `starlte-mac-j313.adt` + this kernel. The same recipe rebuilds `xnu-mac13-v33.xnu` byte for byte from today's kernel |
| `xnu-ios26-health.xnu` | 44c3b7e550adb3e3... | ios26-sd's boot-args (538 bytes) + `starlte-iphone11.adt` (the one inside ios26-sd, verbatim) + this kernel |

The payloads' boot-args are unchanged. The loader appends `sbwdt=` and `sbcrash=` from the catalog
(tools/store/catalog-xnu-health.toml).

## 6. Android: the entry's own device tree (the coordinator's request)

### 6.1 Catalog

The 4th file of an android entry is optional:

```toml
files = ["oneui.kernel", "oneui.ramdisk", "oneui.cmdline", "oneui.dtb"]
```

The 3-file entries work as before. Without a 4th file, `boot_android` takes the old path
byte for byte: S-Boot's FDT, plus the cmdline file and the entry cmdline appended. A 4th file the
store does not have makes the entry "not ready" (`NO ANDROID DTB`), and a start fails back to the
menu. It never falls back silently to S-Boot's DT. Store it with
`sbstore2.py build ... oneui.dtb=PATH:dtb`. UN1CA's
`platform/exynos9810/installer/kernel_dtb/ds_ack_v1_12/G960F-dtb` can be used as it is (a DTBH).

### 6.2 DTBH (Samsung's dtbtool, version 2)

- Offset 0: `"DTBH"`, u32 version (2), u32 count.
- Offset 12: count × 32 bytes `{u32 chip, platform, subtype, hw_rev, hw_rev_end, offset, size, space}`,
  then a zero u32.
- `offset`/`size` locate the entry's FDT, page aligned (2048).
- Every DTBH seen here has one entry: chip 0x2652 (9810), platform 0x50a6, subtype 0x217584da,
  rev 26..255, FDT at 0x800. That holds for TWRP, LineageOS dt.img and UN1CA G960F-dtb.

**Pick**:

1. The chip from S-Boot's root `model_info-chip`.
2. The board revision from S-Boot's `/chosen/revision`, else root `model_info-hw_rev`.
3. The first entry with that chip and `hw_rev <= rev <= hw_rev_end`, else the first with that chip,
   else the first.

A plain FDT is used as it is. Log slot 21 records `chip << 48 | rev << 32 | index << 16 | count`.

### 6.3 What S-Boot changes at run time, and what is carried over

S-Boot's FDT was compared with the DT it was built from (`build/linux-bundle/sboot.dtb`, read back from
the phone, against LineageOS's dt.img and UN1CA's G960F-dtb: same rev26 board DT, node names aside).
Differences explained by S-Boot at run time:

| Where | S-Boot's FDT | The file |
|---|---|---|
| `/chosen bootargs` | S-Boot's whole command line (2130 bytes: androidboot.*, sec_debug.*, serialno, carrier, dram_info, ...); the file's own args are dropped or rewritten (`selinux=permissive` becomes `enforcing`) | a template (`console=ram root=/dev/ram0 ...`) |
| `/chosen linux,initrd-start/-end` | S-Boot's ramdisk (u32) | placeholders 0x82000000 |
| `/chosen revision` | u32 26 | — |
| `/chosen serial` | u64 | — |
| `/memory@80000000`, `@C0000000`, `@E1900000`, `@880000000` | 4 banks | none |
| root `model_info-system_rev` | `"026"` | — |

Every other difference (energy model, `/firmware/android/fstab`, `rkp`/`tima` status, abox node
names) comes from the DT sources being different builds, not from S-Boot.

### 6.4 The merge (`fdt_merge_sboot`)

The file's FDT is the base:

- Its `/chosen` properties are replaced by **all of S-Boot's**. bootargs gets " " + the cmdline file +
  the entry cmdline (+ the log fragment), the same append as the old path. Its `/chosen` subnodes
  are kept.
- Its memory nodes are dropped and **S-Boot's** are copied in.
- Root properties S-Boot has and the file lacks are added (`model_info-system_rev`).
- Everything else is the file's, byte for byte and in order: phandles, the reservation map,
  boot_cpuid.
- New property names are appended to its strings block.
- Then `linux,initrd-*` point at the ramdisk, as before.
- Output at EDIT_FDT (0xAD000000); the file is loaded to LINUX_DTB (0xAE000000, up to 16 MiB).

Checked on the host for UN1CA and LineageOS by an independent parser (`check_dtmerge.py`):
1490 / 1399 nodes, /chosen = S-Boot's 5 properties, 4 memory nodes, every other node identical.

## 7. Settings record (store block 2) and compatibility

```
  0 "SBSETTNG"  8 u32 version 1  12 u32 CRC32 of bytes 16..255
 16 u32 seq  20 u32 timeout_s  24 u32 flags  28 u32 0
 32 default_key[24]  56 last_key[24]  80 next_key[24]
104 u8 crash_mode  105 u8 crash_detail  106 u8 crash_action  107 u8 crash_timeout (s / 10)
108 u8 wdt_mode  109 u8 0  110 u16 wdt_s  112..255 zero
```

**The flags word: one definition** for the loader (`shim/starlte/store.h`), Engineer OS v2
(`starbite/libsb/store.h`, which the coordinator aligns to this) and `sbstore2.py`:

| Bits | Name | Values |
|---|---|---|
| 0 | `SET_SD_STORE` | read the microSD's store at boot |
| 4-5 | `SET_POST_SHIFT` 4, `SET_POST_MASK` 3 << 4 | 0 `SET_POST_FULL` (default), 1 `SET_POST_QUICK`, 2 `SET_POST_OFF` (3 reads as full) |
| 8-9 | `SET_LOG_SHIFT` 8, `SET_LOG_MASK` 3 << 8 | 0 `SET_LOG_QUIET` (default), 1 `SET_LOG_NORMAL`, 2 `SET_LOG_VERBOSE`, 3 `SET_LOG_DEBUG` |

So every existing record (flags 0 or 1) means a full POST and quiet boots, as the user wants.
`SET_POST_OF(flags)`, `SET_LOG_OF(flags)`, `sbset_set_post/log` read and write them.

- **Old records** have zeros from byte 104, and **every new field's 0 is its default**: full POST,
  quiet, crash detection on, detail 1, menu, 60 s, watchdog per entry. The version stays 1.
- The loader's `settings_save` (menu, Engineer Lite, recovery) keeps bytes 112..255 of the record it
  read, for a future tool. `sbstore2.py settings --from` keeps everything it does not change.
- An **older loader** reads the new record fine. When it rewrites it (the menu remembering the last
  entry), the flags word, and so the POST and log levels, is kept; bytes 104.. (the crash settings)
  go back to their defaults. Harmless.
- Engineer OS v2's settings editor writes the flags bits above. It should also keep bytes 104..255 as
  it found them (note for its owner).
- **Catalog**: the header's last u32 (+28, always 0 before) points to `SBCATEX1`. Older loaders
  ignore it, and a catalog without it gives empty fragments and no watchdog.
- **Store**: a store with `crashlog` works with older loaders. They never look at directory flags,
  and the entry is not in the v1 view. Engineer OS lists store entries: it should not CRC-check
  flag-1 entries (note for its owner).

## 8. Host tests

```sh
sh tools/loader-test/run.sh                    # the Mac's clang, ASan + UBSan
limactl shell starbite -- env CC=gcc sh $PWD/tools/loader-test/run.sh
```

- **host_test.c (252 checks)**:
  - CRC32
  - settings: old record = defaults, round trip, damage, clamps, sbstore2's block as the loader reads it
  - crash log: header and report round trip, the ring wrapping at 17 reports, damage, the log tail
  - the decision table: 21 cases
  - watchdog counts: 30 s = 23803, 60 s = 47607, 82 s = 65063 = the maximum, clamps
  - rings: linear, a full Linux ring, the full 0x3F00 pmsg ring, bad ones
  - `sb_line_edit` removal and append, `sb_arg`/`sb_num` on bootargs, dates (leap days, 2100),
    ramoops record time and panic line
  - DTBH pick: UN1CA, no match, a plain FDT, truncated, empty
  - merges: UN1CA with and without extra, LineageOS; refuses one byte short; repeatable
  - the Linux DTB INFORM3 patch: once, then nothing left, INFORM2 untouched
  - catalog.c parsing sbstore2's catalog: defaults, overrides, an old catalog, a damaged extension
  - SBMAINT1: 17 headers made and judged by `tools/maint/sbmaint.py` (`gen_maint.py`: a valid one, one
    with dtb-rescue, one damage per R4 rule); `sbm_check` agrees on every verdict and field;
    `sbm_slot_b` against `slot_b_offset`; `sbm_order` against `sbmaint.choose` (seq, tie, prev)
  - the shared flag bits (masks, setters keep the other bits)
  - (2026-11-07) the watchdog's arguments: XNU boot-args filled to 589 bytes drop `sbwdt=60` (not
    armed), the last `sbwdt=` wins, Linux `tmr_atboot`/`tmr_margin` after the DTB's `tmr_atboot=0`, a
    dropped margin, a later `tmr_atboot=0`
  - (2026-11-07) POST's key machine: Volume Up held from power on never opens the log (a 10 ms bounce
    is not a release; a real release then a press does), a debounced press opens it once, a 30 ms blip
    does not, sparse polls, log level 2-3 with fastboot's Up held (no hold at the end), quick POST,
    Volume Down skip once, the end decision (failure screen / read pause / straight on), the read
    pause's times, the failure list in display order
  - (2026-11-07) the crash parser and `cl_excerpt` on `fixtures/linux-oops-panic.txt`,
    `linux-sysrq-panic.txt`, `xnu-panic.txt`: message, kernel, task, uptime, the number of message /
    register / trace lines, single lines' kinds, through a ramoops zone and the pmsg ring, the excerpt
    keeping registers and trace within 3824 bytes, prefixes, junk
- **check_dtmerge.py (29 checks)**: section 6.4's rules against the merged files.
- **test_sbstore2.py (13 tests)** (linux-pmos's expected watchdog is now 0, as the catalog's
  `[defaults.linux] wdt = 0` of 1f28633):
  - settings: old block, the C block, unknown bytes kept, errors
  - the C crash log decoded, the `crashlog` command, a damaged report
  - the decoding tables are the same text in C and Python
  - the catalog extension as catalog.c parsed it equals `catalog_ext()`; field errors
  - build: `crashlog` entry volatile, kept on `--import`, not on card stores, `--no-crashlog`
- **Fixtures**:
  - `tools/loader-test/fixtures/un1ca-G960F-dtb`, copied from the VM.
  - `sboot-fdt-scrubbed.dtb`: S-Boot's FDT with serialno, ap_serial, em.did, nonce, bootloader, sales
    and carrier codes, regions and `/chosen/serial` blanked, same length (`scrub-sboot-fdt.py`).
  - When the main checkout has them: `build/android/lineage/dt.img` and
    `build/linux/exynos9810-starlte.dtb`.
- `tools/usb-fastboot/run-host-test.sh` passes: 315 checks (with `oem maint`, `oem maint prev`, a refused
  `oem maint rescue`, a bad variant, `oem boot-next maint-rescue` without a catalog entry).
- The installer's tests that use sbstore2.py still pass: 111 tests.
- **tools/xnu-health-test/run.sh (911 checks, XNU's side, section 5.4)**:
  - `starlte_health.h` comes straight out of the 0399 patch.
  - Constants against the loader's: INFORM values (hw.h), WDT and PMU addresses and bit 23 (wdt.h),
    the mailbox (recovery.c), the ring format (sbfmt.h).
  - crash.c's two needles open XNU's headers byte for byte.
  - For every value the loader can arm, `sb_wdt_count` → XNU's hardware timeout equals the loader's
    ms, and the feed period is at most a third of it.
  - The no-`sbwdt=` fallback against S-Boot's masked state; `sbcrash=` parsing and the reset each
    mode asks for; the crash marker (cut safely, never `panic(`).
  - Dumps written by the patch's own pmsg writer and read back by `sb_ring_text`, `sb_find`,
    `sb_line_at` and `sb_classify`: panic with return to the loader, panic then watchdog, a full ring,
    the early-panic fallback, the sweetreboot timer, a watchdog reset. The reason line is checked.
    The mailbox bytes stay intact.
  - When the main checkout has the live payloads: their boot-args go through the loader's own
    `sb_line_edit`, as `boot_xnu` does it, with the proposal's fragments. On mac13-v33 and
    ios26-sd, `sbcrash=loader` and `sbwdt=60` are appended, and the test entries' removals (`rd`,
    `sdrw`, `sdlog`, `sweetreboot`, console) are made. On ios26-sdt (593 bytes) both tokens are
    dropped: hazard 2.

## 9. Needs the phone

**health3 (2026-11-07), first:**

- A. A normal boot shows only the logo (no text, no bar) with the star's glow and the dots, for about
  8 s, then the menu's intro takes the logo over without a jump (log slot 116: why 0xff, bands 2) and
  slot 245 has flag 8 and RAM about 6000 ms.
- B. Volume Up pressed during POST: the live log, the RAM line counting MiB and percent; at the end
  the summary for 2 s or while Volume Up is held; releasing then goes to the menu (not fastboot).
- C. Volume Up held from power on: the logo (no log), then the loader's fastboot as before.
- D. Volume Down during POST: the rest skipped, straight to the menu.
- E. Only if a red check happens on its own (a real fault, e.g. touch not answering; gold ones such as
  a missing crashlog entry never interrupt): the failure screen, Power continues.
- F. The panic test again (`echo c > /proc/sysrq-trigger` in pmOS): the new report: kernel version,
  uptime, task, the call trace; Volume scrolls, Bixby pages, Bixby held changes the level, the footer
  numbers match; REPORT #n equals `sbstore2.py crashlog live --level 3`'s `--- report #n`.
- G. The font size: readable at arm's length (else `star_fb_mini` can grow to 4 x 4 font pixels:
  60 columns).

**Earlier items:**

1. The full POST's real time, and that windows B and C and the 0x55/0xAA passes are as free as
   believed. Watch: an OS that misbehaves after a full POST (the ECT lesson).
2. DISPLAY: whether the frame-done pending bit latches without the interrupt enabled. If it never
   does, the line is a harmless gold "NO FRAME SEEN"; drop the latch test then.
3. The panel comes back exactly after the full POST and the crash screen (v7 intro capture, log slot
   116).
4. **Whether a watchdog reset reaches S-Boot and then the loader, or ends in upload mode**: the
   biggest unknown (LINUX-REBUILD-PLAN 4.3). If upload mode: set WATCHDOG OFF.
5. Whether ramoops (0xFED10000) survives S-Boot after a panic=10 SWRESET and after a watchdog reset.
   TWRP showed it survives SWRESET.
6. Linux with `s3c2410_wdt.tmr_atboot=1 tmr_margin=60`: the driver takes the running watchdog over
   (dmesg "watchdog active, reset enabled").
7. S-Boot's `sec_debug.reset_reason` codes on this phone (the S/D/P/B/T names are unverified).
8. The settings page and SAVE (one UFS write), the crash log writes (2 to 3 per OS start).
9. One UI 8 with its own DTB: boots, and `/proc/device-tree/chosen/bootargs` = S-Boot's + the
   cmdline file.
10. XNU 0399 (section 10.1):
    - XNU can read and write 0x10060000 from its mapping.
    - The thread call keeps a 30 s watchdog from firing.
    - `sbtesthang`'s reset reaches the loader (point 4, now for XNU).
    - `sbcrash=loader`'s SWRESET lands in the loader with the pmsg dump intact.
    - Ventura at its desktop, then idle with the display asleep, is never reset by a 60 s watchdog.

## 10. Flash and test plan (for the coordinator; the user only presses buttons)

**Images**:

- **BOOT `out/xnu-boot-health3.img` (2026-11-07, sha256 a7e942e7ca7d123a..., loader Image f218e182...)**: this branch's loader
  (POST logo / live log / failure screen, RAM budget with reads, the crash report, the watchdog
  argument check) with exactly health2's other parts: template `out/xnu-boot-batch8.img` (header and
  DTBH), `--append build/boot-batch/payload-v7test.sweetxnu`, `--ramdisk build/brand/menu-v7.sbm`:
  `python3 -I tools/starlte-pack-boot.py pack out/xnu-boot-batch8.img shim/starlte/out/xnu_boot.Image
  out/xnu-boot-health3.img --append build/boot-batch/payload-v7test.sweetxnu --ramdisk
  build/brand/menu-v7.sbm` (the same command with health2's loader gives health2, 51dbc26d..., byte for
  byte). Roll back with health2.
- BOOT `out/xnu-boot-health2.img` in this worktree. It is the loader of this branch plus the iOS
  payload of `out/xnu-boot-batch8.img` (`build/boot-batch/payload-v7test.sweetxnu`, sha256
  29568ace...) and the menu ramdisk `build/brand/menu-v7.sbm` (6b69ce08...), with batch8's header
  and DTBH.
- Roll back with batch8 (7c248609...).

**Order**:

0. Keep batch8 for the rollback.
1. Rebuild the UFS store from the phone's live copy with the new catalog. That adds `crashlog` and
   the `SBCATEX1` table. The store can go before or after BOOT, because an older loader ignores both.

   ```sh
   python3 -I tools/store/sbstore2.py build build/store/ufs-store-health1.img --medium ufs \
       --import <live store pull> --catalog tools/store/catalog.toml [oneui.dtb=<G960F-dtb>:dtb ...]
   diff <(python3 -I tools/store/sbstore2.py catalog-keys <live pull>) <(python3 -I tools/store/sbstore2.py catalog-keys build/store/ufs-store-health1.img)
   ```

   Flash it the usual way. The settings block (block 2) is kept by the flash tools.
2. Flash BOOT.
3. **Boot 1, untouched**:
   - Expect the full POST screen for about 9 s, then the menu.
   - Photograph POST.
   - Read `fastboot oem log pmsg`. Expect the lines `POST full ...`,
     `crash: crash log started` (the first start), and `crash: ... NO OS WAS RUNNING`.
4. **Boot 2**: hold Volume Down when POST starts. Expect "SKIPPING THE REST OF POST", then the menu.
5. **Settings**: hold Bixby, then STARTUP SETTINGS: POST QUICK, SAVE, CONTINUE. The next boot shows
   the quick lines. Set it back to FULL.
6. **Clean run**:
   - Start postmarketOS. Expect a quiet boot: no kernel text, the initrd's lines remain.
   - `reboot`. Expect no crash screen.
   - pmsg shows `launch linux-pmos #n watchdog 60 s`, then `... CLEAN RESTART`.
7. **Panic**:
   - In pmOS as root, run `echo c > /proc/sysrq-trigger`.
   - With a kernel that has steps 6g and 10 (LINUX-REBUILD-PLAN 7.5): within about 1 s the
     Starbite panic screen (red "STARBITE: LINUX KERNEL PANIC", "sysrq triggered crash", gold
     "RESTARTING IN 10 S", the kernel log below). Older kernels: the last picture stays.
   - After about 10 s the phone resets, then POST, then the crash screen KERNEL PANIC ("panic:
     sysrq triggered crash"); with step 10 the level-1 RESET row reads SWRESET.
   - Kernel f797be6b missed that reset: the screen came, but panic()'s mdelay() countdown waits in
     WFE, which this SoC wakes about 12x too late with interrupts off, so the 60 s watchdog reset it
     (CLUSTER1_WDTRESET). 6a605a1f spins instead (kernel step 11) and arms the watchdog for
     panic_timeout + 5 s at the panic as a backstop (LINUX-REBUILD-PLAN 7.6-7.9). A RESET row of
     CLUSTER1_WDTRESET after about 15 s means the backstop caught a remaining stall.
   - 6a605a1f still reset about 2 minutes late. 8775991b counts down on the panel itself, timed by the
     counter and by the panel's frame starts, then writes SWRESET. It shows a diagnostics row (CLOCK, TE,
     WDT0/WDT1, SWRESET), and the stopped CPUs keep spinning (LINUX-REBUILD-PLAN 7.10-7.12).
   - Try Volume Up (levels 2 and 3), Bixby (older lines), Power (menu).
   - The menu must show no countdown.
   - `python3 -I tools/store/sbstore2.py crashlog live --level 3`.
8. **Hang**:
   - In pmOS as root, run `exec 3>/dev/watchdog; sleep 300`. Opening it stops the kernel's own
     pings, and nothing writes.
   - About 60 s later the phone resets, and the crash screen shows HANG (WATCHDOG), RESET:
     CLUSTER1_WDTRESET.
   - **If it lands in upload mode instead**: Volume Down + Power 7 s, set WATCHDOG OFF in the
     settings, and record it.
9. **Forced restart**: in any OS, hold Volume Down + Power 7 s. Expect FORCED RESTART (warn).
10. **Android**: start Android (LineageOS / One UI). It is unchanged at levels 0 and 1.
11. **One UI 8 with its DTB**: needs an entry with 4 files.
    - Check that it boots to the home screen.
    - Over adb: `cat /proc/device-tree/chosen/bootargs` (S-Boot's line + the cmdline file),
      `ls /proc/device-tree | grep memory` (4 nodes), `cat /proc/device-tree/model_info-system_rev`.
12. **Log level 2 (verbose)**:
    - Set it in the settings, then start pmOS. Expect kernel text on the panel (loglevel 7).
    - Set it back to 0 (quiet).
13. **Starbite Maintenance** (section 12; needs the store entries `maint*` or the SBMAINT partition):
    - Hold Bixby, then STARBITE MAINTENANCE. With no source: red "NO MAINTENANCE SYSTEM: ...".
      Otherwise Power: the phone resets and starts it (screen rows "STARBITE MAINTENANCE" / "STARTING
      SLOT A (SEQ N)" or "STARTING FROM THE OS STORE"; log slot 10 = 0x710/0x711/0x712).
    - In fastboot: `fastboot oem maint`, then (from a running Linux) `reboot-to maint`.
    - Hold Bixby during a maint boot: the request is cancelled and the recovery menu comes up.

### 10.1 XNU (0399): watchdog feeder and crash return

**Needs**:

- the health loader on BOOT (steps 0-3 above; build/boot-batch/xnu-boot-health2.img was not flashed
  as of 2026-11-06);
- the settings' WATCHDOG on PER ENTRY (the default) and CRASH DETECTION on;
- the store rebuilt with `mac13-health.xnu=out/xnu-mac13-health.xnu:xnu` (from this worktree) and the
  test entries of tools/store/catalog-xnu-health.toml (part 3).

**The test entries** (X1-X4) boot the mac13 payload with `!rd !sdrw !sdlog !sweetreboot` removed from
its boot-args:

- no root device: XNU stops at "waiting for root device" with every kernel thread running;
- no card writes: every write in starlte_sd.c needs `sdrw=1`;
- no UFS root;
- `!a5fb_noconsole !sweetquiet`: the kernel text is on the panel.

A crash in these tests therefore cannot hurt Ventura's card root. Each is started by key (`fastboot oem
boot-next KEY`, or Engineer's Start an OS), never by payload name. The user only presses buttons; the
coordinator reads the results (`python3 -I tools/store/sbstore2.py crashlog live --level 3` in the
loader's fastboot, and photos of the panel).

| Step | Entry (cmdline) | Expect | If not |
|---|---|---|---|
| X1 fed | `xnuh-feed` (wdt 30) | Lines `STARBITE HEALTH: sbwdt=30: cluster 1 watchdog WTCON 0xff39 WTDAT 23803 (29998 ms) ... fed every 9999 ms`, `watchdog feeder running`, `sbcrash=loader: INFORM3 ... -> 0x12345670`, then "waiting for root device". **No reset for 3 minutes.** Then the user holds Volume Down + Power 7 s: the loader's screen says FORCED RESTART (warn), which shows the boot-time INFORM3 = normal | A reset after about 30 s: feeding does not reach the hardware. Read the crash report's WDT1 registers, then set `wdt = 0` for XNU |
| X2 hang | `xnuh-hang` (wdt 30, `sbtesthang=60`) | At about 60 s: `sbtesthang: feeding stopped after N feeds`. About 30 s later the phone resets on its own. POST, then the crash screen: **HANG (WATCHDOG)**, "cluster 1 watchdog not fed for 30 s", RST_STAT bit 23 (0x00800000) | **Upload mode** or a black screen: Volume Down + Power 7 s. Record it (the open question of section 9, point 4) and keep `wdt = 0` for XNU |
| X3 panic | `xnuh-panic` (wdt 30, `sbtestpanic=60`) | At about 60 s the phone resets at once (SWRESET). Crash screen: **KERNEL PANIC**, reason `panic(cpu N caller 0x...): STARBITE: sbtestpanic=60: test panic ...`. Level 2 shows the marker line `STARBITE: xnu crash v1: return loader, uptime ... s, watchdog 30 s fed ... times` and the panic log head | Lands in TWRP: INFORM3 was not normal. Pull `/sys/fs/pstore/pmsg-ramoops-0` there and check its marker line says `return loader` |
| X4 stuck panic | `xnuh-panic-spin` (wdt 30, `sbtestpanic=60`, **no** sbcrash) | The panic text stays on the panel for about 30 s, then the watchdog resets the phone: KERNEL PANIC, marker `return none (spin)`, RST_STAT bit 23 | As X2 |
| X5 soak | `mac13` with `files = ["mac13-health.xnu"]`, `wdt = 60` (part 2 of the proposal) | Ventura's desktop as before. Use it about 10 min, then leave it idle about 20 min with the display asleep: **no reset**. The persistent log has `fed every 19999 ms` | A reset with HANG (WATCHDOG) while working: set mac13's `wdt = 0` and send the report (the feeder starved?) |
| X6 (optional) | ios26 the same way with `ios26-health.xnu` | as X5 | as X5 |

Roll back: point `mac13` back at `mac13-v33.xnu` and set `wdt = 0` for it. The test entries are
hidden. The loader and S-Boot are not touched by any XNU step.

## 11. RAM used (added)

| Address | Size | Use |
|---|---|---|
| 0x9A000000 | 16.3 MiB | the panel during the full POST, then during the crash screen |
| 0x9B100000 | 64 KiB + 7.5 KiB | POST: the logo unpacked (word A8, star RGB at +64 KiB) |
| 0x9F030000 | 64 KiB | the crash report's composed lines |
| 0x9F040000 | 96 KiB | the crash report's rows |
| 0x9F060000 | 16 KiB | the kinds of the shown text's lines |
| 0x9F000000 | 32 KiB | snapshot of 0xFED10000..0xFED18000 |
| 0x9F008000 | 4 KiB | snapshot of 0x80000000 (sec_debug) |
| 0x9F010000 | 64 KiB | the crash log (UFS DMA) |
| 0x9F020000 | 2 × 16 KiB | the ramoops zones as text |
| 0xAF400000 | 4 KiB | the launch record (after the OS is loaded; above UFS DMA's buffers, below RAM_END) |
| windows A, B, C | section 2.3 | full POST patterns |

Log page slots: 10 = 0x710..0x713 for Starbite Maintenance (slot A, slot B, store, fastboot), 19
(launch; bit 16: the watchdog's arguments did not fit, not armed), 20 (INFORM3 pairs patched), 21
(Android DTB pick), 200/201, 202..212 and 240..244 (POST), 245 (how POST was shown, RAM ms), 216
(recovery, unchanged), 220..223 (crash).

## 12. Starbite Maintenance in the loader (docs/MAINTENANCE.md 4.2)

Implemented: R1, R2, R3, R4, R5, R7. Not done: R6 (`download` key) and R8 (rescue falling back to SBMAINT).

- **R1, the recovery item "STARBITE MAINTENANCE"** (after Rescue):
  - `star_maint_ready()` reads the GPT and the two slot headers (header checks only), else looks for
    a ready catalog entry `maint`. With neither: red "NO MAINTENANCE SYSTEM: SLOT A <why>, SLOT B
    <why>, NO MAINT ENTRY IN THE STORE", and the menu stays.
  - Otherwise a second prompt picks the variant: **Power** `maint`, **Volume Up** `maint-prev` (the
    other slot), **Volume Down** `maint-rescue`, **Bixby** back. This replaces the spec's "Volume Up
    held while selecting": the menu's Volume Up already moves the highlight.
  - Then the mailbox key (key first, `SBNEXT2!` last, as Rescue) and INFORM3 normal + SWRESET.
- **R2, the early keys** `maint`, `maint-prev`, `maint-rescue`: taken with `rescue` by `rec_next_take()`
  right after the Bixby window, and the mailbox is cleared before anything starts.
  - Bixby held in that boot cancels the request, logged.
  - The start comes where Rescue's does, after `boot_info`: no POST, no crash screen, no menu.
  - A failure opens the recovery menu with the reason in red.
- **R3**: `fastboot oem maint [prev|rescue]` places it from UFS and starts it (`FB_ACT_BOOT`, log slot 10
  = 0x713). `oem boot-next` / `oem reboot-to` now accept `rescue`, `maint`, `maint-prev`, `maint-rescue`
  without a catalog entry.
- **R4, the sources**:
  1. **The GPT partition `SBMAINT`**, found by name.
     - Slot A is at byte 0, slot B at `(size / 2) & ~0xFFFFF`.
     - Each header gets every check of `sbmaint.parse_header`, in `sbm_check` (sbfmt.c, host-tested
       against the reference).
     - The order is by seq, A first on a tie; `maint-prev` reverses it.
     - A slot is read whole (`total` ≤ 0x04FF0000) into STAGE 0x8C000000. Every present section's
       CRC-32 and the kernel's `ARMd` are checked. The DTB must take the bootargs and have initrd
       properties.
     - Any failure moves on to the next slot.
  2. **The store's catalog entry** `maint` (`maint-rescue`, else `maint` plus `sb.rescue=1`), through
     `boot_linux` with the entry's files and cmdline.
  3. Else: "NO MAINTENANCE SYSTEM: ...".
- **R5, the start**: placed as the rescue start does it.
  - Placement: kernel at RAM_BASE + text_offset (≤ OS_RD), initrd at OS_RD, the DTB (dtb-rescue for
    `maint-rescue` when present) copied to EDIT_FDT and appended into LINUX_DTB, `starbite,rtc-epoch`,
    the microSD rails.
  - Bootargs get `" <header cmdline> sb.maint=1 sb.maint.src=a|b sb.maint.seq=N
    sb.maint.from=next|fastboot [sb.rescue=1] sb.serial=<S-Boot's androidboot.serialno>"`. From the
    store, only `sb.maint.from=... [sb.rescue=1] sb.serial=...`, because the entry's cmdline has the
    rest.
  - Screen rows 0-1, pmsg `STARBITE: maintenance from slot a seq N <version>`, log slot 10 = 0x710 /
    0x711 / 0x712 / 0x713. INFORM3 is left alone.
  - A launch record (key `maint`, flag 16): the next start judges how it ended. The launch before it
    is judged first, so a crashed test OS is still reported when maint starts next.
  - No watchdog and no log fragments for maintenance.
- **R7, the return key**: mailbox `SBNEXT2R` + `KEY\0RET\0`.
  - At the early take it becomes a plain `SBNEXT2!` + KEY, so the menu's boot-next starts KEY.
  - Right before KEY's handoff (`launch_go`, only for that key) the mailbox is re-armed with RET. If the
    test OS does not overwrite the ramoops ring at 0xFED17FE0 (mainline Linux's ramoops does, unless
    its DTB keeps ramoops off as SBMAINT's does), its reset comes back to RET.
