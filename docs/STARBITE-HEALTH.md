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
tree (section 6).

**Status: built and host-tested. Nothing here has run on the phone.** The loader builds with
`-Werror` and has no ABS64 relocations. The host tests (section 8) pass with the Mac's clang
(ASan + UBSan) and with gcc in the VM. Section 9 lists what only the phone can confirm. Section 10
is the flash and test plan.

## 1. Files

| File | What |
|---|---|
| `shim/starlte/sbfmt.c/.h` | **No hardware** (host-tested): the settings record codec, the crash log format, the reset-reason decision, watchdog math, ramoops ring text, command line edits, dates |
| `shim/starlte/dtmerge.c/.h` | **No hardware** (host-tested): DTBH selection, the S-Boot merge for Android device trees, in-place DTB cell patch |
| `shim/starlte/post.c/.h` | POST levels, the full POST and its screen, Volume Down to skip |
| `shim/starlte/crash.c/.h` | Snapshot at start, launch record, evaluation, report, crash screen, the recovery menu's report list, fastboot text and raw |
| `shim/starlte/wdt.c/.h` | Arming cluster 1's watchdog right before an OS starts |
| `shim/starlte/xnu_boot.c` | The order (section 4.1), log-level fragments, watchdog arguments, Linux INFORM3 patch, Android 4th file |
| `shim/starlte/store.c/.h` | Settings record with the new fields (bytes kept), `crashlog` entry access (`SF_VOLATILE`) |
| `shim/starlte/catalog.c/.h` | The catalog extension `SBCATEX1` |
| `shim/starlte/recovery.c` | "STARTUP SETTINGS..." and "CRASH REPORTS..." |
| `shim/starlte/menu.c`, `menu6.c`, `menu7.c` | No auto-start countdown right after a crash |
| `shim/starlte/fastboot.c`, `fastboot_dev.c` | `fastboot oem log crash` and `crashraw` |
| `shim/starlte/fbtext.c/.h` | Glyphs `( ) ' " @ \| ; & $ { } ^ ~ \` \\` for log lines; `star_fb_muted()` |
| `tools/store/sbstore2.py` | Settings fields, `settings-show`, `crashlog IMG\|live`, the catalog extension, the `crashlog` store entry |
| `tools/store/catalog.toml` | `[defaults.linux/android/xnu]`, per-entry overrides, field documentation |
| `tools/loader-test/` | Host tests and fixtures (section 8) |

## 2. POST (request a)

### 2.1 Levels and keys

| Setting `post_level` | What runs | Time |
|---|---|---|
| **full** (0, the default; every record older than 2026-11-06 reads as full) | all 16 checks on their own screen | about 8 to 10 s, see 2.4 |
| quick (1) | the 11 checks of 2026-11-05, as lines over S-Boot's splash | about 0.3 s |
| off (2) | nothing; the menu brings UFS and touch up when it needs them | 0 |

- **Change it**: hold Bixby at boot, then **STARTUP SETTINGS... → POST AT BOOT** (Power cycles FULL,
  QUICK, OFF), then **SAVE**. From a Mac: `sbstore2.py settings OUT --from BLOCK --post quick`.
- **Skip it once**: hold **Volume Down** during POST. The rest of the run is marked SKIPPED, the
  boot goes on, and the setting does not change. Volume Down is debounced (two reads 5 ms apart)
  and polled after every check and every 4 MiB of the RAM test.
- **Bixby** during POST opens Starbite Recovery afterwards, as before.
- **The Bixby check comes before all of this and is unchanged**: `rec_bixby_window` still uses only
  the key GPIO, the timer and fbtext. The settings record is read after it, and so is POST.

### 2.2 The full POST

The screen is black, with the title, a progress bar, the 16 check lines (✓ / ✗ / !, name, detail,
ms), a summary line, then the failures (red) and warnings (gold) as a list. The hint is "VOL DOWN:
SKIP BIXBY: RECOVERY". At the end it pauses 3 s after a failure, 1.5 s after a warning and 0.5 s
when clean. Volume Down or Power cuts the pause short. The whole panel is saved first (0x9A000000)
and put back after, so menu v7's intro still captures S-Boot's splash.

| Check | How (full) | Bound | Fail (red) | Warn (gold) |
|---|---|---|---|---|
| RAM | address and ~address over windows A, B, C (2.3); each window written whole, then read whole (aliasing shows), then 0x55/0xAA over A while time is left | **6 s budget**, checked every 4 MiB | any bad word (**fatal**) | Volume Down cut it short |
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

RAM 6 s (budget) + timer about 1.5 s (waits for two RTC edges) + touch 0.2 s + display 0.06 s +
storage, battery, rest about 0.2 s + the closing pause 0.5 s = **about 8.5 to 9 s**, plus the
400 ms Bixby window. `POST_RAM_MS` in post.c sets the RAM share.

### 2.5 Results

- `post_last()` for the menus. The recovery menu shows the failures.
- Log page 0x91fff000: slot 200 `"POST" << 32 | mode << 24 | n << 16 | fails << 8 | warns`,
  slot 201 total µs, **slots 240 + id** per check (`state << 56 | value << 32 | µs`; ids in post.h).
  The old per-check slots 202..212 are no longer written: with 16 checks they would reach
  recovery's slot 216.
- pmsg ring: `STARBITE: POST full 8712ms fail 0 warn 1` and one line per failure or warning.

## 3. Log levels (request b)

One global setting, `log_level` 0..3, in the settings record: **0 quiet (the default)**, 1 normal,
2 debug, 3 verbose. To change it: Starbite Recovery → STARTUP SETTINGS → LOG LEVEL, or
`sbstore2.py settings OUT --from BLOCK --log 2`.

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

| Method | 0 quiet | 1 normal | 2 debug | 3 verbose |
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
- **XNU**: boot-args are first-match, so appending cannot undo the payload's `-v` or
  `a5fb_noconsole=1`. A fragment token `!name` therefore first **removes** every `name` and
  `name=...` token from the payload's boot-args (`sb_line_edit`). The daily payloads are already
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
| XNU | pmsg ring 0xFED14000 | `STARBITE: XNU panic (sweetreboot), panic log head:` + console tail, or `STARBITE: sweetreboot timer (no panic)` (xnu/patches/pending-mac13/0312, series-12377/0349) |
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

### 4.4 The crash screen

- **Rows 0-9**: "STARBITE: THE LAST START CRASHED" (red) or "...ENDED BADLY" (gold); the OS name
  and key; the kind; the reason over two rows; when (the OS's own clock for Linux panics, else
  detection time UTC); started; the evidence source; RST_STAT bits; S-Boot's verdict.
- **Level 1** (summary): "SAVED AS REPORT #n".
- **Level 2**: plus the last log lines, wrapped to 38 columns. Bixby pages back.
- **Level 3**: plus the registers (RST_STAT, INFORM2/3, PMU WDT disable/mask, WDT1 WTCON/WTDAT/WTCNT,
  WDT0, S-Boot rst/pwron/pwroff, sec_debug magic, launch watchdog, flags), then the log.
- **Keys**: Volume Up / Down change the level, Bixby shows older lines, Power continues.
- **Timeout**: `crash_timeout` (default 60 s, counted from the last key; 30/60/120/300/600 in the
  settings page). The screen is capped at 10 minutes whatever the setting. Nothing waits for ever.
- The panel is saved and put back around the screen.
- Then, by `crash_action`: the boot menu (default), Starbite Recovery, or **retry once**: the same
  entry starts again with `CLF_RETRY`. A crash of the retried start goes to the menu.
- Starbite Recovery → **CRASH REPORTS...** lists the saved reports, newest first, and opens any of
  them on the same screen at level 3.

### 4.5 Settings (Starbite Recovery → STARTUP SETTINGS, or `sbstore2.py settings`)

| Row | Values | Record byte |
|---|---|---|
| POST AT BOOT | FULL, QUICK, OFF | 104 |
| LOG LEVEL | 0 QUIET .. 3 VERBOSE | 105 |
| CRASH DETECTION | ON, SAVE ONLY (no screen), OFF (no launch record, no INFORM3 patch) | 106 |
| CRASH SCREEN SHOWS | 1 SUMMARY, 2 + LOG, 3 ALL | 107 |
| AFTER A CRASH | BOOT MENU, RECOVERY, RETRY ONCE | 108 |
| CRASH SCREEN WAITS | 60, 120, 300, 600, 30 s | 109 (seconds / 10, 0 = 60) |
| WATCHDOG | PER ENTRY, OFF, 30 S, 60 S, 82 S | 110 mode, 112 u16 seconds |

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
- **Android (Samsung 4.9)**: S-Boot gives `androidboot.debug_level=0x4f4c` (LOW): panics and
  watchdog resets reboot normally (no upload mode) and S-Boot passes KPON/WPON. Nothing to do.
- **XNU**: `starlte_panic_to_recovery()` (bsd/dev/starlte_pmsg.c) writes INFORM3 = 0x12345674, so
  panics land in RECOVERY. **Patch to specify, not done here**: write 0x12345670 when the boot-arg
  `sbcrash=loader` is present (the loader could append it in `[defaults.xnu]`). The pmsg dump stays
  as it is, and the loader reads it.
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
| **XNU** | Booted with `wdt=-1 -wdt_no_pmu`: nothing feeds the Exynos watchdog. **Patch to specify, not done here**: in bsd/dev/starlte_pmsg.c, parse `sbwdt=N` (the loader appends it with `wdt_feed = "xnu"`), map 0x10060000 with `ml_io_map`, and from a `thread_call` every N/3 s write WTCNT (+8) = WTDAT (+4). Do not pet once `panic` starts (the sweetreboot path resets anyway). Until then, XNU's software `sweetreboot=N` timer is the hang detector, and the loader reads its dump | `[defaults.xnu]`: wdt 0 |
| **Windows / UEFI** | Off. The woa firmware and Exynos9810Ufs.sys own cluster 0's watchdog through the RWD1 handshake (NOTES 2026-10-26); a second owner would fight it | none |
| **Bare** (Engineer OS) | Off unless libsb pets it | none |

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
 16 u32 seq  20 u32 timeout_s  24 u32 flags (1 = card store at boot)  28 u32 0
 32 default_key[24]  56 last_key[24]  80 next_key[24]
104 u8 post_level  105 u8 log_level  106 u8 crash_mode  107 u8 crash_detail  108 u8 crash_action
109 u8 crash_timeout (s / 10)  110 u8 wdt_mode  111 u8 0  112 u16 wdt_s  114..255 zero
```

- **Old records** have zeros from byte 104, and **every new field's 0 is its default**: full POST,
  quiet, crash detection on, detail 1, menu, 60 s, watchdog per entry. The version stays 1.
- The loader's `settings_save` (menu, Engineer Lite, recovery) keeps bytes 114..255 of the record it
  read, for a future tool. `sbstore2.py settings --from` keeps everything it does not change.
- An **older loader** reads the new record fine, but rewriting it (when the menu remembers the last
  entry) zeroes bytes 104.., so it falls back to the defaults. Harmless.
- Engineer OS (starbite/libsb) only reads the record.
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

- **host_test.c (125 checks)**:
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
- **check_dtmerge.py (29 checks)**: section 6.4's rules against the merged files.
- **test_sbstore2.py (13 tests)**:
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
- `tools/usb-fastboot/run-host-test.sh` still passes: 302 checks.
- The installer's tests that use sbstore2.py still pass: 111 tests.

## 9. Needs the phone

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

## 10. Flash and test plan (for the coordinator; the user only presses buttons)

**Images**:

- BOOT `out/xnu-boot-health1.img` in this worktree. It is the loader of this branch plus the iOS
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
   - After about 10 s the phone resets, then POST, then the crash screen KERNEL PANIC ("panic:
     sysrq triggered crash").
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
12. **Log level 2**:
    - Set it in the settings, then start pmOS. Expect kernel text on the panel (loglevel 7).
    - Set it back to 0.

## 11. RAM used (added)

| Address | Size | Use |
|---|---|---|
| 0x9A000000 | 16.3 MiB | the panel during the full POST, then during the crash screen |
| 0x9F000000 | 32 KiB | snapshot of 0xFED10000..0xFED18000 |
| 0x9F008000 | 4 KiB | snapshot of 0x80000000 (sec_debug) |
| 0x9F010000 | 64 KiB | the crash log (UFS DMA) |
| 0x9F020000 | 2 × 16 KiB | the ramoops zones as text |
| 0xAF400000 | 4 KiB | the launch record (after the OS is loaded; above UFS DMA's buffers, below RAM_END) |
| windows A, B, C | section 2.3 | full POST patterns |

Log page slots: 19 (launch), 20 (INFORM3 pairs patched), 21 (Android DTB pick), 200/201 and 240+
(POST), 216 (recovery, unchanged), 220..223 (crash).
