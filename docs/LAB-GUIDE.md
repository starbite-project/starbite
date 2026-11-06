# Starbite Lab: user guide (v2.1)

Starbite Lab is the Engineer OS: tests, hardware information and tools that can change anything on the phone.
Start it from the boot menu: the **Lab** button in the dock (or the Starbite row). It runs on its own,
no OS, no card needed.

## Moving around

| Key | Does |
|---|---|
| Volume Up / Down | move the highlight (in number fields: change the digit) |
| Power (press and release) | open / select |
| Power held 0.6 s | back (in a test: stop the test) |
| Bixby short / long | back / the tab's first page |
| Touch | works everywhere: tap rows, swipe right = back |

The **Dashboard** lists every tab under TABS. Leave Lab with **Dashboard → Exit to the boot menu**
(it clears any pending one-shot boot and restarts into the menu).

## The tabs

| Tab | What is in it |
|---|---|
| Dashboard | live tiles (CPU MHz, hottest temperature, GPU, battery, current, RAM, uptime), device info, the tab list, Exit |
| Tests | **Full health check**, CPU stress, Memory stress, Thermal test, Battery tests, Storage test, Display stress, Touch grid, **GPU tests**, plus the quick tests (keys, touch paint, display) |
| Power | battery and charger, battery graph, battery tests, PMIC regulators and register dump, PMIC/charger writes, reboot and power |
| Clocks | CPU, clocks, temperatures, **every thermal sensor**, GPU, GPU tests, real-time clock |
| Storage | UFS controller and health, partitions, GPT viewer, partitions and LUs, sector viewer (any disk, edit), speed and storage tests, microSD |
| Starbite | OS store, catalog, settings record, **settings editor** (default OS, timeout, POST level, log level), start an OS |
| Logs | Lab's log, the boot log, loader handoff, test history, audit log (every risky action), retained records, kernel command lines |
| Tools | armed mode, register peek/poke, memory dump/fill, PMIC and charger writes, partitions and LUs, OS store, settings editor, reboot to anything |

## Tests (safe: they only read, except where marked)

Every test: a setup page (Power changes an option), a live page, a result page. **Power held stops
any test at any time.** Results go to the test history; **Save report** writes them into the OS store.

| Test | What it does | Notes |
|---|---|---|
| Full health check | 15 quick checks in ~30 s: timers, PMIC, touch, battery and gauge, temperatures, every CPU core, RAM, storage, display, GPU | start here |
| CPU stress | integer, FP, NEON and cache work on the cores you pick, results checked | offers to switch on the 4 big (M3) cores first (a tier 3 step) |
| Memory stress | pattern tests over the free RAM window (16/64/126 MiB) | |
| Thermal test | idle, then load for 30 s..60 min, then cool-down; every sensor once a second, throttling detected | "Stop at" 70–95 °C or **no limit** |
| Battery tests | health; discharge under load with a capacity estimate; voltage sag and internal resistance; charge | discharge takes 2–30 min |
| Storage test | read speed + verified re-read + random reads | read only |
| Display stress / Touch grid | fill/scroll/flip speed; touch every cell | |
| GPU tests | power up the Mali-G72, identify it, job test, an 8×8 colour pattern checked and shown, stress, clock levels | powering up sets the GPU voltage (tier 1); clock levels above the first are tier 3 |

**No limit**: the temperature limit can be switched off for the stress tests, through a warning that
shows what still protects the chip (the SoC's own 115 °C hardware trip, read live; never seen firing).
With no limit only you stop the heat: watch the temperature.

## The dangerous parts: tiers

Lab can change anything the hardware allows. Every change shows a **warning screen first** (what will
happen, the target, the old and new value, how to undo it), then asks for more confirmation the more
dangerous it is. **Cancel is always the default; holding Power cancels.**

| Tier | What you must do | What it covers (examples) | Worst case |
|---|---|---|---|
| 0 | nothing | reading anything | none |
| 1 | warning + hold (slide, or Volume Up 1.5 s) | OS partitions (Android, Windows, card), saving reports, GPU voltage for the tests, the settings | an OS no longer boots until re-installed |
| 2 | + **type the target's name** | BOOT (the Starbite loader), RECOVERY, the live OS store, erasing a partition, peripheral clock gates | the menu or an OS no longer starts: needs Maintenance/TWRP or the Mac |
| 3 | + **hold Power 3 s**, and a **verified backup first** for storage | EFS (IMEI), PARAM, KEYSTORAGE, DTB, the partition table, the boot areas (S-Boot), PMU, CPU/GPU clocks and voltages, M3 core power-up | **the phone may not boot at all**, lose its IMEI/network, or need Download mode and the Mac to recover |
| 4 extreme | full-screen **red** warning, type **"&lt;target&gt; I ACCEPT"**, slide, **hold Power 5 s**; storage backup unless you pick "no backup" on a second red screen | S-Boot's calibration table (ECT), Lab's own memory, secure-world addresses, raw writes across partitions, unknown PMIC registers, GPU clocks outside the table | can **brick** the phone (a broken S-Boot area), corrupt Android's boot, or crash Lab instantly |

Rules that keep you safe:
- **Read the warning screen.** It says exactly what will change and how to undo it.
- **Tier 3 and 4 make a backup first.** It goes into the OS store (`bak-<NAME>-<n>`); the screen says where. Note it.
- **Undo** (Tools) restores the last register, PMIC or charger write. Storage is restored from its backup (Partitions and LUs → restore).
- **Never write** the boot areas (S-Boot, boot LUs), EFS, PARAM or the partition table unless you know exactly why: these are the ones that can stop the phone from starting or cost the IMEI.
- **Secure world**: Lab tries the access and reports the fault; the hardware blocks it, it cannot be changed.
- Every tier 3/4 action is written to the **audit log** before it runs (Logs → Audit log).

## If something goes wrong
- Lab frozen: hold **Volume Down + Power 7 s** (forced restart); the loader records it.
- The menu no longer starts: hold **Bixby** at power-on → Starbite Recovery → Starbite Maintenance; the Mac restores from the backups.
- Nothing starts at all: Download mode (Volume Down + Bixby + Power), then the Mac (the installer's restore).

## What Lab cannot do (yet)
No shader programs on the GPU (fills and writes only), no CPU overclocking (reads clocks and throttling
only), no modem / Wi-Fi / camera / audio / sensor-hub tests (no drivers), no restore of a broken S-Boot
from inside Lab (the backup is there, the Mac restores it). Details: docs/ENGINEER-OS.md sections 8 and 13.
