# Engineer OS v1 (EN1 + EN2 + EN3)

Engineer OS is Starbite's bench and management tool, its own image started from the catalog
(`docs/STARBITE-MASTER-PLAN.md` §4.2 libsb, §4.5 Engineer OS). It runs on **libsb**, the Starbite
core, as a `bare` image: the loader loads it from the OS store and jumps to it. The loader's
EngineerMode (`shim/starlte/em.c`) stays in the loader as **Engineer Lite** (hold Bixby in the
menu): the rescue tool that works when the store is broken, and the only one that writes anything.

**v1 writes nothing to storage, and no regulator.** Every change Engineer Lite makes by hand (touch
rails, card rails, the settings record, a register poke) is a change card here, logged as a dry run.

Status (2026-11-05, v1.1): **boots on the phone** from the OS store (libsb comes up, touch works:
coordinator report after the sb_dcache_all GOT fix). Everything else is checked on the host only
(unit tests, relocation test, the keys-only tour of every page, preview frames): see section 11 for
the status of each page and section 12 for the phone checklist.

v1.1 (this change): Volume + Power alone reach and leave every page (hold Power = back, the
Dashboard's TABS rows switch tabs), a one-step **Exit to the boot menu** (Dashboard and Tools), the
touch and keys tests can be left without Bixby, the Brightness write asks first, the RAM test labels
match the real window (0xA0100000, 126 MiB, ECT left alone), the register policy moved to `policy.c`
and is unit-tested, and the build is reproducible and self-checking.

## 1. Layout

| Path | What |
|---|---|
| `starbite/libsb/` | the core: `crt0.S` (header, entry, self-relocation, vectors, PSCI core stub), `reloc.c`, `mmu.c`, `sys.c` (timer, keys, reset, power off, boot-next, PSCI, probes, arena), `log.c`, `fdt.c`, `acpm.c` (S2MPS18), `hsi2c.c`, `touch.c` (S6SY761), `max77705.c`, `tmu.c` (temperatures, clocks), `ufs.c` / `store.c` / `catalog.c` / `sd.c` (read-only copies of the loader's), `fence.c` (write fence, dry-run), `crash.c`, `sb7.c` + `assets.S` (embedded fonts), sbui (`sbui*.c`) |
| `starbite/engineer/` | the application: `main.c` (chrome, screens, navigation), `tabs.c`, `pages_em.c` (em.c's pages), `pages_sb.c`, `dash.c`, `tests.c`, `armed.c`, `policy.c` (register peek / poke policy, pure), `build.sh`, `preview/*.png` |
| `starbite/host/` | the host preview (`host.c` fake hardware, `preview.sh`, the keys-only `tour`), `selfcheck.sh` |
| `starbite/tools/` | `mkassets.py` (font container), `check-image.py` (header, CRC, size budget), `reloc-test.c`, `unit-test.c` |
| `starbite/out/` | build output (ignored): `engineer.img`, `engineer.img.sha256`, `engineer.elf`, `engineer.map` |

## 2. Build

In the Lima VM (native aarch64 gcc), from the repo root or as part of the loader build:

```sh
limactl shell starbite -- sh -c 'cd <checkout>/shim/starlte && sh build.sh'   # loader, then Engineer OS
limactl shell --workdir <checkout> starbite sh starbite/engineer/build.sh     # Engineer OS alone
```

`build.sh`:
1. packs `starbite/out/sbassets.sb7` from `build/brand/menu7.sb7` (all 29 fonts and the line icons;
   the checkout's or the main checkout's; `SB7=` overrides). Without one it prints a note and exits 0,
   so the loader build never fails because of Engineer OS;
2. compiles libsb and the app with `-fpie -fvisibility=hidden -Wall -Wextra -Werror`, FP/SIMD allowed;
3. links a static PIE at 0 (`libsb/image.ld`, `ld -pie --no-dynamic-linker -z text`) and the same
   objects at 0x40000000 for the test;
4. refuses undefined symbols and any relocation other than `R_AARCH64_RELATIVE`;
5. `check-image.py`: pads to 4 KiB, writes `image_size` and the CRC32 at +88, checks "ARMd",
   `text_offset` 0x80000, the branch at +0 to `entry`, `_image_end` = file size (the .bss is in the
   file), every relocation entry RELATIVE and inside the image;
6. `reloc-test` (host): runs the image's own `sb_reloc_check` on the image mapped at 0x40000000 and
   compares it byte for byte with the 0x40000000 link (only the patched header fields, `.rela.dyn`,
   `.dynamic`, `.dynsym` and GOT[0] may differ); calls `sb_selftest()` inside the relocated mapping
   (tables of function and string pointers, 141663 expected); relocates again at a random address
   and checks every word moved by the delta; checks a corrupted entry is refused.

Also, since v1.1:
- before anything is compiled for the phone, `tools/unit-test.c` (host, aarch64 VM) tests the pure
  logic and stops the build on a failure: the string builder and CRC32; `sb_map_kind` (RAM test
  window non-cacheable end to end, 0xaf800000..0xbfffffff unmapped); the register policy (every CMU
  block, MIF/INT, PMU, ACPM mailbox and SRAM, fuses and DRAM never pokeable and named as such; an
  exhaustive 4 KiB sweep of 0..0x20000000 finds exactly the 5 allowlisted pages at tier 1); the
  write fence on an S9-like GPT (never-touch partitions, tiers, ranges) and that **`sb_write` refuses
  at every tier** (v1 dry-run); the UFS SCSI allowlist (only TEST UNIT READY, INQUIRY, START STOP,
  READ(10); 14 write opcodes refused before any register is touched); the key state machine
  (Power selects on release, held 0.6 s = back once, Bixby short / long, Volume repeat, keys held
  across a reset ignored). Mutations of the policy, the Power timing and the opcode list were
  checked to make it fail;
- `-ffile-prefix-map`, and a scan that refuses an image containing build-host paths
  (`/Users/`, `/home/`, ...);
- the pre-relocation check also covers `sb_dcache_all` and fails if a function cannot be
  disassembled (an empty listing would pass silently);
- `check-image.py`: at most 32 MiB, at least 96 MiB of arena left below 0xa0000000 when loaded at
  0x92080000, `sb_vectors` 2 KiB aligned, the entry-path symbols inside the image;
- `sb.h` static asserts: the RAM test window starts at or above 0xa0100000 (S-Boot's ECT at
  0xa0000000 is never written) and ends inside the non-cacheable map; the arena ends at 0xa0000000;
- `out/engineer.img.sha256`, and the asset container's sha256 in the log (it is the one input not
  in the repository: `build/brand/menu7.sb7`);
- `sh starbite/engineer/build.sh --repro` builds twice (the second time into another directory)
  and requires identical images.

Output: `starbite/out/engineer.img` (4.0 MiB, 372 relocations at v1.1).

### 2.1 Host self-checks (no phone)

```sh
sh starbite/host/selfcheck.sh     # build --repro (unit tests + every image check, twice) + the tour
sh starbite/host/preview.sh tour  # the keys-only tour alone
```

The **tour** runs the real application on the host preview and, with Volume and Power only (no
touch, no Bixby), opens every row of every tab (62 pages; other tabs through the Dashboard's TABS
rows), checks that each page drew something and that every drawn pixel reached the panel, then
holds Power four times and checks it is back at the Dashboard. Exit status 0 = all passed.

## 3. Entry contract (what the loader leaves, what crt0 does)

The loader (`shim/starlte/xnu_boot.c` `boot_bare`, `xnu_boot.S`) loads the store entry with
`image_load` at `0x92000000 + text_offset` = **0x92080000** (CRC-checked against the store directory)
and enters it with:

| State | Value |
|---|---|
| EL, MMU, caches | EL1, MMU off, D- and I-cache off; the loader cleaned and invalidated the D-cache by set/way |
| x0 | S-Boot's FDT (0x8a000000), x1-x3 = 0 |
| DAIF | all masked (the loader masked them and never unmasked) |
| CPACR_EL1 | S-Boot's value (FP may trap) |
| VBAR_EL1 | S-Boot's vectors |
| Display | DECON_F trigger free-running (0x1281), the framebuffer at 0xcc000000 scanning out |
| Other cores | off (the loader is single core) |
| Log page | 0x91fff000: "STRLTE01", slot 10 = 0x600, slot 15 = entry, slot 17 = x0 |
| Assets | the BOOT ramdisk is probably still at 0x89000000 but nothing guarantees it: Engineer OS embeds its fonts |

crt0 (`libsb/crt0.S`): mask interrupts; keep x0; `adr _start` = base; own 256 KiB stack in .bss;
FPEN on; `sb_mmu_enable` (plain C, uses no stored pointer, so it runs before relocation);
`sb_reloc_check(base, __rela_start, __rela_end, image_size)` (every entry RELATIVE and inside the image,
`*(base + off) = base + addend`); VBAR = `sb_vectors`; `sb_start` (records the handoff in `sb_boot`,
sets the DECON trigger, logs) → `sb_main`.

Memory map (identity, `libsb/mmu.c`): 0..2 GiB device; 0x80000000..0x9fffffff write-back (FDT, ramdisk,
loader log, image, arena); 0xa0000000..0xa7ffffff non-cacheable (RAM test window); 0xa8000000..0xaeffffff
write-back; 0xaf000000..0xaf7fffff non-cacheable (UFS DMA, catalog scratch); 0xaf800000..0xbfffffff
**unmapped** (RKP, TIMA, secure world); 0xc0000000..0xffffffff non-cacheable (framebuffer
write-combined, pstore / pmsg / boot-next); 0x880000000.. non-cacheable (bank 2, peek only).
The arena (back buffer, spare buffer, page pools, icon cache) runs from the image end to 0xa0000000.

Faults: inside a probe (`sb_probe32`, `SB_SAFE_MRS`) a synchronous abort or trapped instruction is
counted and skipped; anything else shows the crash screen (5x7 font straight to the framebuffer:
ESR class, ELR / FAR, ELR as image offset for `engineer.map`, all registers, the last log lines; also
written to pmsg). Keys there: Power = menu, Volume Up = Engineer OS again, Volume Down = power off.

Exit: **Exit to the boot menu** (on the Dashboard under LEAVE, and in Tools): a dialog, then the
boot-next mailbox is invalidated (a stale key would skip the menu) and the PMU resets with INFORM3
normal: S-Boot, the loader, its menu. **Tools > Reboot and power** has the other modes (Engineer OS
again, recovery, download, power off). Starting
another OS (**Starbite > Start an OS**, the iOS options page) writes the boot-next mailbox at
0xFED17FE0 (`"SBNEXT2!"` + key[24]) and resets through the PMU (INFORM2 0x12345678, INFORM3 normal,
SWRESET); the loader starts that entry once without the menu (`shim/starlte/menu.c boot_next`).
Power off is PS_HOLD low (PMU +0x330C bit 8); with a cable in it reboots to the menu.

## 4. Deploy (not done yet: no flashing in this change)

Catalog entry (`tools/store/catalog.toml`, already added):

```toml
[[entry]]
key = "engineer"
name = "Engineer OS"
version = "Engineer OS 1 · read only · dry-run writes"
category = "starbite"
method = "bare"
tag = "beta"
files = ["ufs:engineer.img|sd:engineer.img"]
root = "none (RAM only)"
```

Store entry name: **`engineer.img`** (type `image`). From TWRP, with the live store pulled first:

```sh
tools/store/store-pull.sh build/store/live.img
python3 tools/store/sbstore2.py build build/store/new.img --medium ufs --catalog tools/store/catalog.toml \
    --import build/store/live.img engineer.img=starbite/out/engineer.img:image
diff <(python3 tools/store/sbstore2.py list build/store/live.img) <(python3 tools/store/sbstore2.py list build/store/new.img)
diff <(python3 tools/store/sbstore2.py catalog-keys build/store/live.img) <(python3 tools/store/sbstore2.py catalog-keys build/store/new.img)
tools/store/store-flash.sh build/store/new.img
```

The menu needs a loader with the menu7.c change below (the Engineer OS row in the Starbite section
works with any v7 loader; the Lab row / button only with the new one). A test boot without the menu:
`tools/store/boot-next.sh engineer`.

**menu7.c** (`go_lab_os`, `run_lab`): the Lab row and the dock's Lab button start the `engineer` entry
when `star_entry_ready` says it is there (launched like a picked entry); otherwise Engineer Lite, as
before. Holding Bixby always opens Engineer Lite.

## 5. sbui

Back buffer in write-back RAM; every draw marks its rectangle dirty; `ui_present` copies only the
dirty rectangles (merged, at most 24, else their bounding box) to the write-combined framebuffer.
Nothing is read from the panel. Retained pages (`sbui_page.c`): a page is rebuilt from its fill function
and compared item by item with what is on screen; only items whose content changed are redrawn
(a live page that changes one value redraws one row). Scrolling moves the back buffer's rows
(`ui_scroll_rows`) and draws only the strip that comes into view; flings decelerate per frame. Widgets:
status bar (time from the RTC + counter, battery %, an ARMED pill), large-title header with back,
tab bar with icons, grouped cards (headers, key/value, rows with switches and chevrons, big values,
monospace lines, notes, custom items), dialog, hold-Power ring, hold-to-confirm slider (the change
card), toast, line chart. Fonts: menu v7's Inter, Inter Tight and JetBrains Mono at exact sizes, with
kerning (`SB7ASSET`, embedded); UTF-8 plus the loader font's one-byte codes (catalog strings). No glass.

Keys everywhere: Volume moves (repeats while held), Power selects (on release), **Power held 0.6 s =
back** (once; nothing on the release), short Bixby back, long Bixby the tab's root (at a root: the
Dashboard). Without touch, the tab bar is replaced by the Dashboard's **TABS** rows (hold Power at
any tab's root to come back). Touch: tap, drag, fling, swipe right = back, the tab bar. Every
modal follows the same keys: dialogs (Volume picks, Power confirms, hold Power cancels), the change
card (hold Volume Up 1.5 s to slide, hold Power cancels), the tier 2 keypad, the display test
(Power next, hold Power leaves). Exceptions, because the keys or touch are the thing under test:
**Touch screen** (Volume or Bixby leaves, short Power clears, hold Power leaves) and **Keys** (hold
any one key 1.5 s, Bixby 1 s, or tap Back).

## 6. Pages

| Tab | Pages |
|---|---|
| Dashboard | TABS (every other tab, for keys), LEAVE (Exit to the boot menu); live tiles (A55 MHz, M3 MHz / off, hottest °C, GPU °C + MHz, battery % / V / input, current mA, RAM, uptime), current graph; Device information, SoC and CPU, Memory map (+ the FDT's reserved-memory, live), Reset and boot reason, Battery graph |
| Tests | Core bring-up test, Memory test (16 MiB or the 126 MiB window at 0xA0100000, non-cacheable), Memory bandwidth (DRAM and cached), Display test (11 fields, 20 s cap per field), Display panel, Brightness (AOR, asks once per session), Touch screen (10 fingers), Keys, Speed test, Test history |
| Power | Battery and charger, Battery graph, PMIC regulators, PMIC register dump, Touch rails (tier 1, dry-run), Reboot and power |
| Clocks | CPU, Clocks, Temperatures, GPU (Mali-G72), Real-time clock, Timer |
| Storage | UFS host controller, UFS device and health, Partitions, GPT viewer (both headers, CRCs, fence tier per partition), Sector viewer, Speed test, microSD card, Card store at boot (tier 1, dry-run) |
| Starbite | OS store, Catalog, Settings record (+ the boot-next mailbox), Start an OS (boot-next), Write fence, Apple, iOS boot options, Android 17, Linux, Windows 11 |
| Logs | Engineer OS log, Boot log (pmsg), Loader handoff (log page + this image's entry state), Test history, Audit log, Retained records, Kernel command lines, Registers |
| Tools | Armed mode, Register peek, Exit to the boot menu, Reboot and power, Stress / Radio / Installer ("coming"), About |

Every em.c page is there. Differences: BOOT's iOS payload, its boot-args and the UEFI bundle live in
the loader's RAM (shown as such; iOS options are decoded from the catalog's command lines); the intro
statistics are not shown; the RAM test and bandwidth use libsb's windows, not 0x92000000.

Test history (EN2): each result is kept for the session and appended to the pmsg RAM ring as
`SBENG1 T PASS|FAIL <name> | <detail>` (with a `SBENG1 B <n>` line per start). The ring survives a warm
reset, so the next start reads earlier sessions back. Not a power-off: history on the card needs EN4.

## 7. Host preview

```sh
sh starbite/host/preview.sh            # all scenes -> starbite/engineer/preview/*.png
sh starbite/host/preview.sh "armed peek"
```

The real app and sbui code compiled for aarch64 Linux on `starbite/host/host.c`: register blocks,
DRAM windows, pmsg and the loader's log page are anonymous mappings at their real addresses with
seeded values; PMIC, fuel gauge (a moving current), touch, UFS (a GPT like S9 #1's), store (the real
catalog from `catalog.toml`) and card are fakes; EL1 system register reads are emulated. A script of key
presses and taps runs on a fake clock. Every shot prints how many panel pixels differ from the back
buffer (0 = the dirty rectangles covered every change); `S:` shots also redraw the scrolled page whole
and compare (0 = incremental scrolling is exact). All 26 scenes report 0 and 0 (`hub`: the Dashboard's TABS rows, new in v1.1).

## 8. Armed mode (EN3, dry-run)

| Tier | Allows (in v1: as change cards, nothing written) |
|---|---|
| 0 (default) | read only |
| 1 (armed) | peripheral rails within policy, registers in allowlisted blocks, OS partitions (SYSTEM, VENDOR, ODM, A16DATA, WINESP, WINDOWS, USERDATA incl. the Starbite store), the card |
| 2 (second unlock: type the partition name) | BOOT, RECOVERY, UP_PARAM |
| never | boot LUNs, EFS, CPEFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA0-2, DTB, DTBO, the GPT/PIT head and tail; registers in CMU blocks (PERIS, PERIC0/1, FSYS0/1, APM, CMGP, DPU, G3D, AUD, CPUCL0/1, 0x1a000000..0x1a3fffff TOP/CORE/BUSC), MIF / DREX / DMC 0x1b800000..0x1bffffff, the ACPM/APM mailbox 0x14100000 and APM SRAM, the whole PMU (DVFS, reset), CHIPID / fuses, DRAM; anything not on the poke allowlist |

Arming: battery ≥ 30 % (refused below; allowed with a warning when the gauge does not answer), hold
Power 2 s (progress ring), then the slider (touch, or hold Volume Up 1.5 s). Disarms after 5 minutes
without input, when the page it was armed on is left, and on reboot (RAM only). While armed: a red
14 px border on every frame (nothing draws into it), "ARMED T1 · DRY RUN · m:ss" in the status bar.
A change card shows what, target, old value, new value, backup location and the steps (backup,
verify, write, verify, log); after the slide the audit log and the Starbite log get a `DRY RUN` line.
Storage changes also go through `sb_write` (`libsb/fence.c`), which classifies the target against the
live GPT and logs "fence: would write ...". Poke allowlist: GPIO_PERIC0, HSI2C USI03, GPIO_FSYS1,
GPIO_CMGP, HSI2C USI_CMGP03. Register reads: em.c's peek blocks plus GPIO_FSYS1, DWMMC2, DECON,
DSIM, and DRAM outside the secure carve-outs.

## 9. What Engineer OS writes, and what it never writes

Writes (all the loader's own behaviour): the ACPM IPC queue (one command slot, the indices, INTGR0),
the RTC read latch (RTC 0x02 UPDATE), HSI2C controller registers, the touch and MAX77705 pin mux /
SYSREG USI mode only when they differ, UFS host setup (`ufs_up`, as the loader), the vs_hci registers
during the speed test (restored), DSIM / panel AOR (Brightness, until the next OS), DECON trigger,
pmsg ring, boot-next mailbox, PMU reset / PS_HOLD, the second cores through PSCI.

Never: CMU blocks (gates are read; a closed one is reported, e.g. "card clocks off"), regulators (the
touch and card rails are read; switching them is a tier 1 dry run), MIF / INT, DREX / DMC, PMU DVFS,
fuses, storage (ufs.c sends read opcodes only; sd.c never writes; there is no settings_save).

## 10. Untested and risky (first boot checklist)

- First boot done (boots, libsb up, touch works). On any problem: pull pmsg, every `eng:` line
  (base, fdt, relocs) and any `CRASH` line with ELR as an image offset (`engineer.map`).
- The MMU attributes (MAIR / TCR as the loader's) and the unmapped 0xaf800000..0xbfffffff hole.
- The touch controller re-init without CMU writes (needs the loader's menu to have started touch);
  without it Engineer OS is keys-only.
- The MAX77705 bus without Engineer Lite's CMU_CMGP gate writes (S-Boot drives that bus, so its clocks
  should run).
- The microSD page reads only a card the loader already powered ("Card store at boot" on).
- Frame rate on the A55: not measured. A scroll frame moves the viewport (~13.7 MB) in write-back RAM
  and copies it to the write-combined panel; the host preview proves which pixels change, not how fast.
- The menu7.c Lab change is built (loader and the menu7 preview `h7` compile) but not exercised.

## 11. Status per page and test (v1.1)

"HW" = seen working on the phone; "host" = built, drawn and navigated in the host preview and the
keys-only tour (fake hardware), not yet looked at on the phone. Writes: what the page itself writes
beyond reads (section 9); "dry-run" = only a change card and a log line.

| Page / test | Status | Writes |
|---|---|---|
| Start-up, relocation, MMU, crash vectors | **HW** (boots, libsb up) | - |
| Touch input (taps, scroll, tab bar) | **HW** (touch works) | - |
| Keys-only navigation (hold Power = back, TABS rows) | host (tour: 62 pages) | - |
| Exit to the boot menu | host | boot-next mailbox cleared, PMU reset |
| Dashboard tiles, current graph | host | - |
| Device information, SoC and CPU, Memory map, Reset and boot reason | host | - |
| Battery graph, Battery and charger | host | - (MAX77705 reads) |
| Core bring-up test | host (dialog first) | PSCI CPU_ON / OFF |
| Memory test 16 MiB / 126 MiB | host | the RAM test window 0xA0100000.. only |
| Memory bandwidth | host | the RAM test window + an arena buffer |
| Display test (11 fields, 20 s cap) | host | framebuffer |
| Display panel | host | - |
| Brightness | host (asks once per session) | DSIM / panel AOR (S-Boot's command, until the next OS) |
| Touch screen (10 fingers) | host; touch itself HW | - |
| Keys | host | - |
| Speed test (UFS 16 MiB read) | host | vs_hci registers during the test (restored) |
| Test history (pmsg) | host | pmsg ring |
| PMIC regulators, PMIC register dump | host | - (ACPM reads) |
| Touch rails, Card store at boot | host | dry-run |
| Reboot and power | host | PMU reset / PS_HOLD |
| CPU, Clocks, Temperatures, GPU, Real-time clock, Timer | host | RTC read latch |
| UFS host controller, UFS device and health, Partitions, GPT viewer, Sector viewer | host | - (read opcodes only, unit-tested) |
| microSD card | host | - |
| OS store, Catalog, Settings record, Write fence | host | dry-run |
| Start an OS, iOS boot options | host | boot-next mailbox, PMU reset |
| Apple, Android 17, Linux, Windows 11 | host | - |
| Logs: Engineer OS log, Boot log, Loader handoff, Audit log, Retained records, Kernel command lines, Registers | host | - |
| Armed mode (arm, tier 2 keypad, change cards) | host | dry-run (audit + log line) |
| Register peek | host | poke = dry-run change card |
| Stress, Radio, Installer | "coming" pages | - |
| About | host | - |

## 12. Phone checklist (Engineer OS v1.1)

Start it from the boot menu's Lab row / dock (or `tools/store/boot-next.sh engineer`). Only the
phone's buttons and screen are needed; tick what works, note what does not:

1. It starts on the **Dashboard**; the tiles (A55 MHz, temperatures, battery %) change every second.
2. **Keys only**: Volume Down moves the highlight, Power opens the row, **hold Power about 1 s**
   goes back.
3. Volume Down to **TABS**, open **Tests** with Power; hold Power: back at the Dashboard. Same for
   one or two other tabs.
4. **Touch**: tap the tab bar, scroll a long page (Memory map), swipe right to go back.
5. Tests > **Keys**: press each of the four keys (each gets a tick), then hold one key 1.5 s to leave.
6. Tests > **Touch screen**: draw with several fingers; short Power clears; Volume leaves.
7. Tests > **Display test**: Power steps through the 11 fields; hold Power leaves early.
8. Tests > **Memory test** > Quick: 16 MiB PASS. (Optional: Whole window, a few seconds.)
9. Tests > **Speed test**: a MiB/s figure, no error.
10. Storage > **Partitions** lists the GPT; Starbite > **Catalog** lists the entries.
11. Tools > **About**: note the FRAMES average and worst copy time (the frame rate on the A55 has
    never been measured).
12. **Exit to the boot menu** (bottom of the Dashboard, or Tools), confirm: the phone reboots to the
    Starbite menu, not straight into an OS.

Anything odd: reboot to TWRP and pull pmsg (`eng:` and `CRASH` lines) before starting another OS
(Windows clears the ring).
