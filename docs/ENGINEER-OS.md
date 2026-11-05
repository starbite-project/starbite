# Engineer OS v1 (EN1 + EN2 + EN3)

Engineer OS is Starbite's bench and management tool, its own image started from the catalog
(`docs/STARBITE-MASTER-PLAN.md` §4.2 libsb, §4.5 Engineer OS). It runs on **libsb**, the Starbite
core, as a `bare` image: the loader loads it from the OS store and jumps to it. The loader's
EngineerMode (`shim/starlte/em.c`) stays in the loader as **Engineer Lite** (hold Bixby in the
menu): the rescue tool that works when the store is broken, and the only one that writes anything.

**v1 writes nothing to storage, and no regulator.** Every change Engineer Lite makes by hand (touch
rails, card rails, the settings record, a register poke) is a change card here, logged as a dry run.

Status (2026-11-05): built and checked on the host (image header, relocation test, preview frames).
**Not run on the phone yet.**

## 1. Layout

| Path | What |
|---|---|
| `starbite/libsb/` | the core: `crt0.S` (header, entry, self-relocation, vectors, PSCI core stub), `reloc.c`, `mmu.c`, `sys.c` (timer, keys, reset, power off, boot-next, PSCI, probes, arena), `log.c`, `fdt.c`, `acpm.c` (S2MPS18), `hsi2c.c`, `touch.c` (S6SY761), `max77705.c`, `tmu.c` (temperatures, clocks), `ufs.c` / `store.c` / `catalog.c` / `sd.c` (read-only copies of the loader's), `fence.c` (write fence, dry-run), `crash.c`, `sb7.c` + `assets.S` (embedded fonts), sbui (`sbui*.c`) |
| `starbite/engineer/` | the application: `main.c` (chrome, screens, navigation), `tabs.c`, `pages_em.c` (em.c's pages), `pages_sb.c`, `dash.c`, `tests.c`, `armed.c`, `build.sh`, `preview/*.png` |
| `starbite/host/` | the host preview (`host.c` fake hardware, `preview.sh`) |
| `starbite/tools/` | `mkassets.py` (font container), `check-image.py` (header, CRC), `reloc-test.c` |
| `starbite/out/` | build output (ignored): `engineer.img`, `engineer.elf`, `engineer.map` |

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

Output: `starbite/out/engineer.img` (about 4 MB, 361 relocations at the time of writing).

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

Exit: **Tools > Reboot and power** (menu, Engineer OS again, recovery, download, power off). Starting
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

Keys everywhere: Volume moves (repeats while held), Power selects, short Bixby back, long Bixby the
tab's root (at a root: the Dashboard). Touch: tap, drag, fling, swipe right = back, the tab bar.

## 6. Pages

| Tab | Pages |
|---|---|
| Dashboard | live tiles (A55 MHz, M3 MHz / off, hottest °C, GPU °C + MHz, battery % / V / input, current mA, RAM, uptime), current graph; Device information, SoC and CPU, Memory map (+ the FDT's reserved-memory, live), Reset and boot reason, Battery graph |
| Tests | Core bring-up test, Memory test (16 MiB or the 128 MiB window, non-cacheable), Memory bandwidth (DRAM and cached), Display test (11 fields, 20 s cap per field), Display panel, Brightness (AOR), Touch screen (10 fingers), Keys, Speed test, Test history |
| Power | Battery and charger, Battery graph, PMIC regulators, PMIC register dump, Touch rails (tier 1, dry-run), Reboot and power |
| Clocks | CPU, Clocks, Temperatures, GPU (Mali-G72), Real-time clock, Timer |
| Storage | UFS host controller, UFS device and health, Partitions, GPT viewer (both headers, CRCs, fence tier per partition), Sector viewer, Speed test, microSD card, Card store at boot (tier 1, dry-run) |
| Starbite | OS store, Catalog, Settings record (+ the boot-next mailbox), Start an OS (boot-next), Write fence, Apple, iOS boot options, Android 17, Linux, Windows 11 |
| Logs | Engineer OS log, Boot log (pmsg), Loader handoff (log page + this image's entry state), Test history, Audit log, Retained records, Kernel command lines, Registers |
| Tools | Armed mode, Register peek, Reboot and power, Stress / Radio / Installer ("coming"), About |

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
and compare (0 = incremental scrolling is exact). All 25 scenes report 0 and 0.

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

- Nothing has run on the phone. First boot: `tools/store/boot-next.sh engineer`, then pull pmsg:
  every `eng:` line (base, fdt, relocs) and any `CRASH` line with ELR as an image offset.
- The MMU attributes (MAIR / TCR as the loader's) and the unmapped 0xaf800000..0xbfffffff hole.
- The touch controller re-init without CMU writes (needs the loader's menu to have started touch);
  without it Engineer OS is keys-only.
- The MAX77705 bus without Engineer Lite's CMU_CMGP gate writes (S-Boot drives that bus, so its clocks
  should run).
- The microSD page reads only a card the loader already powered ("Card store at boot" on).
- Frame rate on the A55: not measured. A scroll frame moves the viewport (~13.7 MB) in write-back RAM
  and copies it to the write-combined panel; the host preview proves which pixels change, not how fast.
- The menu7.c Lab change is built (loader and the menu7 preview `h7` compile) but not exercised.
