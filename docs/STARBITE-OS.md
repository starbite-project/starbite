# Starbite OS

Track A of `docs/PLAN-2026-11-07.md`. Our own operating system for the Galaxy S9 (SM-G960F, Exynos 9810),
written from the lowest level up on libsb. Not Linux, not based on anything else. Ultra minimal and ultra
low power; it has what you want in an emergency without being called an emergency OS: a flashlight, a
heater, a loud alarm, a clock, honest battery estimates and, later, calls, texts and basic networking.

Status (2026-11-07): design reviewed (section 13); version 0.1 built and host-tested (section 12); not yet
on the phone (section 14 has the test request).

---

## 0. Principles

1. **Nothing runs unless something happened.** Interrupts and timers wake the CPU; between events the boot
   core sleeps and the other seven cores are off.
2. **The screen is drawn only when it changes.** Every screen is mostly black (OLED: black pixels are off).
3. **Safety lives in the kernel, not in apps.** Heat, light, sound and power are kernel services with their
   own limits. An app asks; the kernel decides. A broken app cannot overheat the phone, drain the last
   few percent of the battery, keep the CPU awake or stop the kernel's loop.
4. **Apps are small separate files behind one fixed API.** Call numbers never change meaning.
5. **The same code runs on the Mac.** Everything but the thinnest hardware layer is plain C that the host
   test and the host preview build and run (like the Utility OS).
6. **Zero storage writes by Starbite OS itself in v0.1.** (The loader writes its own launch record and
   settings on every start, as for every OS.) Later writes go into one store entry only (section 6.2).

## 1. Where it sits

```
power key ─► S-Boot ─► Starbite loader (menu, or the default / one-shot entry) ─► starbite-os.img (bare)
   EL1  kernel: event loop, timers, GIC, power, services, UI renderer   │ libsb drivers (display, touch,
   EL0  apps (.sba files) ── svc #0 ──► fixed API                        │ UFS, PMIC, gauge, TMU, ADC, SMP)
```

- Catalog row (`starbite/sbos/catalog-entry.toml`): key `starbite-os`, category `starbite`, method `bare`,
  files `ufs:starbite-os.img|sd:starbite-os.img`, `wdt = 30`, `wdt_feed = "own"`. Not yet in
  `tools/store/catalog.toml`: the catalog is at the loader's limit of 32 rows (`CAT_MAX`); it goes in place of
  a hidden row or after the limit is raised (the main session's choice).
- The loader enters it like the Utility OS (EL1, MMU off, x0 = FDT, panel scanning out 0xcc000000); libsb's
  crt0 relocates, maps memory and calls `sb_main`, the kernel's entry.
- Code: `starbite/sbos/` — `kern/` (kernel), `apps/` (built-in apps), `sdk/` (the app API, MIT licensed),
  `tools/` (asset and app packers), `build.sh`. Host test `starbite/tools/sbos-host-test.c`, host preview
  `starbite/host/sbos-preview.sh`.

## 2. Kernel

### 2.1 Execution model

One address space (libsb's identity map), one kernel at EL1 on cpu0 (an A55). The other seven cores are off
except as **workers** for heavy jobs (the heater), started and stopped through libsb's `smp.c`.

The kernel is an **event loop** over kernel tasks (state machines) and app handlers:

| Task | Wakes on | Does |
|---|---|---|
| `input` | a 16 ms timer while the screen is on; 100 ms (Power key only) while off. v0.2: key and touch EINTs | keys and touch → input events |
| `ui` | input, an app's UI change, a hold animation, the minute | runs the renderer: redraws changed elements, presents dirty rectangles |
| `power` | idle timers, the Power key, battery readings | screen on / off, the reserve, power off (section 4) |
| `battery` | 10 s while on, 60 s while the screen is off | fuel gauge, the model, estimates (section 4.5) |
| `heater` | 4 Hz while heating | the heater's controller (section 8) |
| `sos` | 250 ms while active | the SOS screen and strobe |
| `house` | 1 s while on, 10 s while off | feeds the watchdog |

Within one loop turn: acknowledge interrupts, run every due timer in priority order (heater, power, input,
sos, ui, house, apps), deliver queued app events, draw, then sleep until the next deadline.

### 2.2 Interrupts and the timer

- **The kernel always runs with IRQs masked** (PSTATE.I = 1 at EL1). `wfi` still wakes on a pending
  interrupt; the loop then reads GICC_IAR, handles and EOIs. No interrupt handler runs inside kernel code,
  so there are no lost wakeups and no nesting. Interrupts are taken as exceptions only from EL0 (an app
  runs with I = 0), which returns control to the loop (2.3).
- **GIC-400** (GICv2; DT `reg`: GICD 0x10101000, GICC 0x10102000; XNU used the same): at start the
  distributor is disabled, every SPI disabled and cleared, PPIs 26/27/29/30 disabled and cleared (S-Boot's
  timer may still be armed), SPI targets set to cpu0, priorities 0xa0, the distributor and the CPU
  interface enabled (EnableGrp1, EOImodeNS = 0, PMR = 0xff), and IAR drained to 1023. Non-secure code cannot
  see the groups, so delivery is tested, not assumed.
- **Timer: the EL1 virtual timer** (CNTV, PPI 11 = INTID 27; deadlines in CNTVCT, since CNTVOFF may be non-zero
  and libsb reads CNTPCT). On ARMv8.0 the virtual timer cannot be trapped by EL2 (Samsung's hypervisor), and
  XNU used it on this phone. The vendor kernel takes its clock events from the MCT and only reads the
  counter; CNTP was never armed at EL1 here, so it is not used.
- **Self test at boot:** arm CNTV 1 ms ahead; within 50 ms exactly one INTID 27 must arrive at IAR, then 5 ms
  of silence (an interrupt storm fails the test); then a 10 ms `wfi` must take at least 8 ms (a trapped or
  immediately returning `wfi` fails it). Pass: interrupt mode. Fail: **polling mode** (the same loop with
  `sb_udelay` steps, only built-in apps, no app budget), shown in Settings and logged.
- **Deep idle (v0.3):** PSCI CPU_SUSPEND (DT power state 0x10000, `c2`) loses the core's own timer, so v0.3
  wakes from the MCT (0x10040000, `samsung,exynos4210-mct`; Linux `exynos_mct.c` as the reference).
- **Keys and touch as interrupts (v0.2):** ALIVE GPIO EINTs for the keys, the S6SY761's interrupt line.

### 2.3 Apps at EL0

- An app is an **event handler**. Events are queued per app and delivered one at a time: the kernel starts
  a handler as a fresh **frame** (x0 = the event in the app's mailbox, x1..x30, TPIDR_EL0 and TPIDRRO_EL0
  zero, SP_EL0 = the app's stack top, ELR = the app's entry trampoline, SPSR = EL0t with DAIF clear) and
  `eret`s into it.
- **Every exit from EL0 returns to the loop:** the vector saves the whole frame (x0..x30, SP_EL0, ELR, SPSR,
  ESR, FAR, q0..q31, FPSR, FPCR) and restores the loop's kernel context. Then:
  - `svc #0` (ESR.EC 0x15): the loop runs the call (never blocking, never re-entering EL0), writes x0 and
    resumes the frame; `SYS_RETURN` ends the handler.
  - an interrupt: the loop does its work (the heater, the watchdog...) and resumes the frame later;
  - anything else (EC 0x20/0x24 aborts, 0x22/0x26 alignment, 0x00 unknown, 0x3c BRK, FIQ, SError): the app
    is stopped ("<name> stopped").
- **Budget:** a handler may use 500 ms. Before every `eret` the kernel arms the timer to the earlier of its
  next deadline and the budget's end, so an app in a loop is always interrupted. Over budget: v0.1 closes the
  app ("<name> stopped responding and was closed"); a **Wait** choice comes later.
- **Hardening:** SCTLR_EL1 is written in full at start: UMA = 0 (EL0 cannot mask interrupts), UCT = UCI =
  DZE = 0, SA0 = 1, nTWI = nTWE = 0 (`wfi`/`wfe` at EL0 trap to the kernel), E0E = 0. CNTKCTL_EL1 = 0 (EL0
  cannot see or touch the counters and timers) and PMUSERENR_EL0 = 0. SPSel is pinned to 1. (PAN is not used:
  smp.c copies SCTLR to the M3 cores, which lack it.)
- **Memory:** 28 slots of 4 MiB in the free write-back window 0xa8000000..0xaeffffff. A slot's two 2 MiB
  block descriptors are EL0 read/write and EL0-executable (UXN = 0; EL0-writable memory is never executable
  at EL1) only while that app's frame runs; otherwise they are kernel-only. Every other block of libsb's map
  gets UXN = 1 (a libsb change: kernel memory was EL0-executable before). A switch is a permission-only flip:
  `tlbi vae1is` on both blocks, `dsb ish`, `isb`. The slot: the app file (its header first) at the slot's
  start, the bss, the heap (`SYS_ALLOC`) upwards; the 4 KiB mailbox at the top and the stack below it. With
  2 MiB blocks there is no guard page: a stack overflow can only damage the app's own heap.
- **System calls** check every pointer against the caller's slot (strings must end inside it), check the
  app's permission bits, and are capped per app: timer periods at least 50 ms and 16 timers, `ui_awake` only
  while the app is in front, `SYS_LOG` 20 lines a second.
- On the Mac the same app sources are linked into the preview; the SDK's `sbos_call` calls the dispatcher
  directly. The EL0 entry and exit, the budget and a deliberate fault are a phone test of their own
  (section 14); the host test covers the pure parts (the file check, the pointer checks, the dispatcher).

### 2.4 What the kernel does not have

No processes beyond apps, no app threads, no file system of its own (the store and, later, the card's exFAT
read-only through the Utility OS's reader), no dynamic linking, no networking yet, no POSIX.

## 3. UI

### 3.1 Look and feel

In the spirit of iPhone Assistive Access / Guided Access: few, very large targets, plain words, one thing per
screen, a big **Back** at the top left, a black background. Fonts and colours are menu v7's (libsb's sbui)
plus two large fonts of the same family (`sbos-assets.py`: Inter Tight 300 at 380 px for the clock, 600 at
150 px for big values) and Starbite's own line icons.

- **Lock screen:** the time (huge), the date, battery % and the estimate, three round **hold** buttons:
  **Light** (0.5 s; full-screen white, off after 10 minutes or at a touch), **SOS** (1 s, then a 3 s
  countdown that a hold cancels), **Open** (0.4 s, to home). Holding protects against pocket touches; the
  ring around a button fills while it is held. A later **Emergency call** button needs a hold and a
  confirmation, and is compiled out of test builds (calls are never tested).
- **Home:** 2 columns of large tiles (icon + name, 625 x 600 px), 6 per page: Light, Heat, SOS, Clock,
  Battery, Settings.
- **App screens:** built from a few elements the kernel draws: a title, buttons (full or half width), a big
  value, a text line, a level picker (Off / Low / Medium / High), a toggle, a hold button (a bar fills). Every
  target is at least 210 px tall (about 9 mm).
- **Keys:** Volume moves the focus; **Bixby** selects (a hold element asks for a second Bixby within 3 s);
  **Bixby held** is Back; **Power** short turns the screen off and on; **Power held 1 s** opens the power
  menu (Screen off, Restart Starbite OS, Boot menu, Power off). During SOS and the light a short Power press
  is ignored (the screen stays on).

### 3.2 Declarative screens, drawn on change

Apps never touch pixels in v0.1: between `ui_begin` and `ui_end` they describe their screen; the kernel keeps
the list on the panel, hashes every element and redraws only those that changed, moved or overlap a change.
A ticking clock redraws one element. Later (v0.4): a **canvas** element for the camera's viewfinder.

### 3.3 The panel's own power

- v0.1 keeps libsb's display path: the loader leaves DECON free-running at 60 Hz, so the panel is refreshed
  from DRAM every frame even when nothing changed, and "screen off" is a black screen.
- v0.2: **DECON software trigger** (a frame only per present), brightness through S-Boot's own AOR sequence
  (`F0 5A 5A`, `B1 hi lo`, `F7 03`, `F0 A5 A5`; build/em-research-brightness.md) with dimming after 15 s,
  **display off + sleep in** (DCS 0x28, 0x10) for screen off and **sleep out + display on** (0x11, 0x29).
  Engineer OS's DSI command sender is the starting point. Each is a phone test of its own.

## 4. Power

### 4.1 States

| State | v0.1 | Later |
|---|---|---|
| **Active** | screen on, cpu0 in `wfi` between events, the other cores off | + DECON software trigger, dimming |
| **Screen off** | a black screen (DECON still at 60 Hz), touch ignored, Power key polled at 100 ms: **not** a low-power sleep | v0.2: panel asleep, key EINT wake; v0.3: CPU C2 + MCT |
| **RAM sleep** | (does not exist yet) | v0.3: the SoC's own suspend-to-RAM (DRAM self-refresh), the first 5 minutes after the screen goes off |
| **Off, state saved** | (does not exist yet; v0.1 powers off without saving only at the reserve) | v0.2: after 5 minutes of RAM sleep (settable) |

The screen goes off after 30 s without input (settable "until restart" in v0.1: settings persist only from
v0.2). Things that keep the phone out of Off: heating, SOS, an alarm-clock time in the next 12 hours (until
RTC power-on alarms are verified), **Stay reachable** (later: keeps the modem up), and a charger.

### 4.2 Deepest CPU idle

v0.1: `wfi` with the virtual timer; clocks as S-Boot left them (all cores about 1050 MHz, measured by the iOS
port, NOTES.md 2026-10-30). v0.2: A55 at its lowest table level (455 MHz) while idle through the listed DVFS
path (8.3). v0.3: PSCI CPU_SUSPEND `c2` when the next deadline is more than 2 ms away, woken by the MCT.

### 4.3 RAM sleep (v0.3, research first)

The vendor kernel's suspend-to-RAM (`exynos-pm`, flexpmu: the PMU's system power registers, ACPM putting
the MIF into self-refresh, PSCI CPU_SUSPEND with the system state, the PMU wakeup mask). The watchdog must be
stopped before it and re-armed after it (the vendor driver does this). Designed and phone-tested on its own.

### 4.4 Off with the state saved (v0.2)

1. Before any write: no charger, and the battery above 3 %. Every app gets `EV_APP_SAVE` and may hand over
   4 KiB (`SYS_STATE_PUT`); the kernel adds its own record (settings, alarm times, the battery model, the
   front app). **Services are never restored:** heat, SOS and the light always start off.
2. The records go to the **state slot** (6.2), then a cache flush (SYNCHRONIZE CACHE) and a UFS power-down
   (START STOP UNIT) before power-off, so the data is on the flash and not in the device's cache.
3. Power off. If `sb_power_off` returns (a cable was plugged in at the last moment), the kernel stays on in
   Screen off.
4. **Waking:** the user makes Starbite OS the loader's default entry once (a normal loader setting, with a
   short timeout and the quick POST), so no settings write happens per sleep cycle. Real time from the
   power key: S-Boot's splash, the loader's POST and the image load, about **10-15 s** (the first minutes
   in RAM sleep are the instant path; that is why they exist). Holding Bixby during POST opens Starbite
   Recovery: the lock screen says so.
5. The kernel finds a valid half of the slot and restores; a damaged state (CRC, version) means a fresh
   start; the restore is skipped when the reset reason is not a normal power-on or when Volume Down is held.
6. **Charger plugged into a phone that is off:** S-Boot starts the BOOT image (the loader) in its charging
   mode. The loader / kernel must read the power-on source (S2MPS18 PWRONSRC, as `em.c` already does) and
   start straight into Screen off with a charge indicator. Phone test of its own.

### 4.5 Battery estimates

- Inputs: the MAX77705 gauge, read only: RepSOC, RepCap (0x05), FullCapRep (0x10), Current (0x0A),
  AvgCurrent (0x0B), VCELL, and the battery thermistor (ADC channel 4).
- Per power state (Active, Screen off, Heat) the kernel learns the mean discharge current: a plain mean of
  the first 5 readings, then a moving average of about 32; readings only after 30 s in a state and never
  while charging. Saved in the state slot from v0.2.
- Shown: "at this use about 9 h", "screen off about 30 h". Counted down to the 5 % cut-off, not to 0, and
  reduced in the cold (below 10 °C battery: × 0.8, below 0 °C: × 0.6). Before a state has 5 readings the
  typical value is used and labelled "typical". "Switched off" is shown as typical only (it cannot be
  measured).

### 4.6 Emergency reserve

The reserve exists for Light and SOS. Below 15 % the heater is refused; below 10 % the screen dims (v0.2)
and the lock screen says so; below **3 %** the kernel powers off (v0.1: without saving) unless SOS is on,
which runs down to 2 %. A start below 3 % shows the lock screen with Light and SOS for 60 s, then powers off.

## 5. The app API (fixed numbers)

`starbite/sbos/sdk/sbapp.h` is the definition. `x8` = number; unknown numbers and the reserved groups return
`SBOS_ENOSYS` (-38), so an app can ask "is there a modem?" safely.

| Nr | Group | Calls |
|---|---|---|
| 0..4 | core | `SYS_RETURN`, `SYS_LOG`, `SYS_VERSION`, `SYS_ALLOC`, `SYS_EXIT` |
| 10..29 | UI | `ui_begin`, `ui_title`, `ui_button(id, label, sub, flags)`, `ui_big(caption, value, colour)`, `ui_textline`, `ui_level(id, "Off|Low|...", current)`, `ui_toggle`, `ui_hold(id, label, ms, flags)`, `ui_end`, `ui_toast`, `ui_light(argb)` (full-screen colour), `ui_awake` |
| 30..39 | time | `time_now(struct sbos_time*)`, `timer_set(id, ms, repeat)`, `timer_cancel`, `uptime_ms` |
| 40..49 | power | `battery`, `estimate`, `power_request`, `sysinfo` |
| 50..59 | light | `torch(level)` (`SBOS_ENOSYS` until the LED driver exists), `strobe` |
| 60..69 | heat | `heat_request(level, minutes)`, `heat_status` |
| 70..79 | alarm | `sos(on)`, `vibrate`, `alarm_set`, `alarm_cancel`, `sos_status` |
| 80..89 | state | `state_put`, `state_get` (4 KiB per app) |
| 90..99 | files | read-only card files (reserved) |
| 100..119 | sensors | reserved |
| 120..139 | phone | `phone_state`, `call_dial`, `call_hangup`, `sms_send`, `sms_read` (reserved) |
| 140..159 | network | reserved |
| 160..179 | camera | reserved |

Events: `EV_APP_START`, `SHOW`, `HIDE`, `BUTTON(id)`, `LEVEL(id, value)`, `TOGGLE(id, on)`, `HOLD(id)`,
`TIMER(id)`, `BACK`, `SAVE`, `POWER(state)`, `SERVICE(id)`.

## 6. App files and the state slot

### 6.1 App file (`.sba`)

A static PIE linked at 0 with the 256-byte header (`struct sbapp_header`, the section `.sbapp.head`) as the
first bytes of the image. The kernel copies the **whole file** to the 4 MiB-aligned slot, so the header lies
at the slot's start and every ADRP page offset stays right. All offsets are from the file's start.

| Off | Field | |
|---|---|---|
| 0x00 | `magic[8]` | "SBAPP001" |
| 0x08 | `api`, `header_size` | 1, 256 |
| 0x10 | `id[24]` | a-z, 0-9, '-'; NUL-terminated |
| 0x28 | `name[24]` | printable ASCII; NUL-terminated |
| 0x40 | `permissions` | LIGHT, HEAT, ALARM, FILES, SENSORS, PHONE, NET, CAMERA |
| 0x44 | `entry` | the SDK's trampoline (`bl app_event; mov x8, #0; svc #0`), 4-aligned, inside the file |
| 0x48 | `file_size` | the whole file, header included |
| 0x4c | `bss_size` | zeroed after the file |
| 0x50 | `rela_off`, `rela_count` | R_AARCH64_RELATIVE entries (8-aligned, inside the file; targets inside the file, values inside file + bss) |
| 0x58 | `icon` | an icon id of the system's assets (v0.1); own icons later |
| 0x5c | `stack_size` | 0 = 64 KiB, at most 256 KiB, 16-aligned |
| 0x60 | `crc32` | of the whole file with this field read as zero |
| 0x64 | `color` | the tile's accent |

The check (`appfmt.c`, 64-bit arithmetic) refuses anything outside these rules or larger than the slot (file
+ bss + stack + mailbox ≤ 4 MiB). After the copy the kernel cleans the data cache to the point of
unification and invalidates the instruction cache (slots are reused). **Built-in apps are built-in by
origin** (packed into the kernel image by `mk-sba.py`), not by id: an app from the store or the card with the
id of a built-in is refused, and its permissions are shown once before its first start. The CRC detects
damage; it is not a signature.

### 6.2 State slot (v0.2)

- One store entry `sbos-state`, 1 MiB, **created on the Mac** by `tools/store/sbstore2.py` with the
  `SF_VOLATILE` flag (as the loader's crash log), so the store's whole-entry CRC does not apply to it. On UFS
  only (the settings live there, and cards tolerate power cuts badly).
- Two halves of 512 KiB written alternately: "SBOSSTA1", sequence, length, CRC32, then records (`id[24]`,
  length, data). The valid half with the higher sequence wins (`state.c`, host-tested).
- Writing it needs two reviewed libsb changes: `sb_write_store` must check the open authorisation's own
  range (today it checks only "inside the store"), and a call that rewrites blocks of a volatile entry in
  place. The consent is a **standing record** the user gives once in Settings (shown, revocable), covering
  exactly that entry's blocks; the kernel opens the gate only for that range.
- State writes use force-unit-access or end with SYNCHRONIZE CACHE (4.4).
- The first phone test of these writes is a dry run (`sb_fence_dry_run`) after a store backup.

## 7. Boot speed

Order: libsb's start, `ui_init` (fonts, the 17 MB back buffer cleared), the lock screen drawn and presented,
*then* the GIC and the timer test, touch, the gauge, the built-in apps. The time zone is the home zone
(Helsinki) until the state slot exists. The kernel logs `sbos: first frame at N ms` (counted from power-on
by the counter, so it includes S-Boot and the loader). The image is about 5 MiB (4.3 MiB of it fonts and
icons) for the loader to read.

## 8. Heater

The phone deliberately makes heat (hand warmer, keeping itself above freezing). A kernel service; the Heat
app only asks for a level and shows the status.

### 8.1 How it makes heat

| Level | Load (cpu0 keeps the kernel) |
|---|---|
| Low | cpu1-3 (A55) at 1794 MHz (ACPM domain 2) |
| Medium | Low + cpu4-5 (M3) at 1469 MHz, then 1794 MHz after 60 s if the battery voltage holds (domain 3) |
| High | Low + cpu4-7 (M3), the same ramp; v0.3: + the GPU at 572 MHz (domain 4) |

- **Heat kernel** (`kern/heatk.c`, ARMv8.0): eight independent NEON FMA streams per core, about 1 ms per
  batch, every batch's result compared with a reference (a wrong result stops the heater: an unstable core).
- **1794 MHz** is the M3s' `quad_freq` / cpufreq `policy-max` / `max-freq-wo-ocp` (DT): the vendor's
  all-core level for real workloads. Without the vendor's OCP driver nothing handles the over-current
  warning, so the M3s ramp up and the controller watches the battery current and voltage.
- The M3 cluster needs the vendor PMU defaults before CPU_ON (the six registers of
  `xnu/patches/0304-starlte-m3-cores.patch`, as Engineer OS writes them): moved into libsb
  (`sb_m3_pmu_defaults`), written once per boot before the first M3 start, only for Medium and High.

### 8.2 Safety design

**Sensors** (all read only):
- battery: the battery thermistor (CMGP ADC channel 4, DT `thermal_source = 2`), median of 3, **flag-only
  samples rejected**. There is **no fallback**: the gauge's temperature register holds what S-Boot last wrote.
- skin: the AP thermistor (channel 0) as the best available proxy for the back of the phone;
- SoC: the hottest valid TMU probe; the TMU's hardware trip must be armed (THERM_TRIP_EN on TMU_TOP; and
  TMU_SUB for the GPU in v0.3);
- gauge: VCELL, instantaneous Current (0x0A), %; the charger's input register. **A failed charger read
  means "unknown" and pauses** (libsb's `sb_bat_read` reports "none" on a failed read: the heater reads the
  register itself).

**To start** (each failure is shown): battery thermistor valid and below 38 °C; at start it must agree
within 4 °C with the USB and charger thermistors (channels 1, 3: at rest they sit near the battery's
temperature); SoC valid; trip armed; battery ≥ 20 % (Low) / 30 % (Medium, High); no charger; first use
confirmed on the phone (a hold).

**Cold start:** SoC below 20 °C or battery below 5 °C: the A55s start at 1248 MHz and step up only while VCELL
stays within 150 mV of its resting value (a cold cell sags; the PMIC's sudden-power-loss detection is on).

**Regulation** (4 Hz), by the number of loaded cores (never full on / full off):
- battery band 39 / 40 °C, skin (AP) ≤ 43 °C, SoC soft limit 75 °C: above any of them one core comes off per
  step; below all of them (minus 1 °C) one comes back, up to the level's cores;
- current: |Current| above 3.2 A or VCELL sagging more than 300 mV: one core off.

**Hard stops** (the session ends; a new start needs the user again):
battery ≥ 42 °C; SoC ≥ 85 °C; skin ≥ 46 °C; no valid battery or SoC sample for 3 s; a stuck battery reading
(the same raw value for 60 s while the SoC moved more than 5 °C, or the SoC up 20 °C while the battery moved
less than 0.5 °C in 5 minutes); battery below 15 %; VCELL below 3.45 V; |Current| above 3.5 A; a heat-kernel
checksum error; a worker fault; the session's time is up (default 30 min, at most 60). A charger pauses (not a
stop) and the load resumes when it is unplugged and everything else holds.

**The load cannot outlive its controller:**
- workers check a **heartbeat** that only the controller writes, after a complete valid evaluation; older
  than 1 s: the worker stops and its core turns off;
- after every stop the kernel checks every worker core: PSCI AFFINITY_INFO where the firmware has it (this
  phone's DT lists PSCI 0.1 ids only, so it likely answers NOT_SUPPORTED), else the worker's own slot (it left
  the work, parked or faulted; a core whose start timed out counts as possibly on and hung): a core not off
  within 1.5 s means the clocks are restored and the phone restarts (`sb_reboot`); a worker that stopped by
  itself (the heartbeat) is noticed and started again;
- the kernel's own timers are reserved (apps can never take the last 24 entries of the timer list), and a
  heater timer that cannot be set stops the heater;
- the kernel feeds the watchdog only from its loop: a hung kernel resets within 30 s (catalog `wdt = 30`);
- the TMU's hardware trip (115 °C, armed by S-Boot, never seen firing) stays the last line; nothing writes
  the TMU.

### 8.3 Clocks

- A new libsb call `sb_acpm_dvfs_listed(domain, khz)` accepts only the exact vendor table levels, and only
  for domains 2 and 3 (4 from v0.3); MIF (0), INT (1) and everything else are refused inside libsb, so no
  caller can reach them:
  - A55 (domain 2): 455, 598, 715, 832, 949, 1053, 1248, 1456, 1690, 1794 MHz (1898 and 1950 exist in the
    table and are never asked for);
  - M3 (domain 3): 650, 741, 858, 962, 1066, 1170, 1261, 1469, 1586, 1690, 1794 MHz;
  - GPU (domain 4, v0.3): 260, 299, 338, 455, 546, 572 MHz (DT `gpu_dvfs_table`).
- **"Answered" is not "applied"** (ACPM echoes any request): within 5 ms the kernel reads the PLL back
  (`sb_a55_hz`, `sb_m3_hz`); more than 2 % above the request stops the heater and restores.
- Domain 3 only after the first M3 core has checked in, and back to its restore level before the last M3
  core turns off; afterwards `sb_m3_on()` must read 0.
- **Restore levels are constants** (A55 1053 MHz, M3 1066 MHz), checked by PLL readback; never "what S-Boot
  left" (the M3s read 0 Hz while off, and S-Boot's ~1050 MHz is not a table level).
- Evidence: the iOS port sent { 2, 1690000 } and { 3, 1794000 } on ACPM channel index 0 on every boot and
  measured 1689 / 1792 MHz (NOTES.md 2026-10-30). Sustained full load was never tested.

### 8.4 First run (phone test, with the user present)

Low for 2 minutes with the Heat screen open, then Medium, then High, each stopped by hand; then a hard stop
with the battery limit lowered to the current temperature + 2 °C (a test build).

### 8.5 Why Engineer OS's CPU stress stayed cool (from the code)

1. **The memory kernel dominates the default mix** ("INT + FP + NEON + memory", `lab.c` kern_mask[0]): each
   batch writes and checks 512 KiB per core (`kern.c` k_mem), which stalls the cores on DRAM for most of the
   batch. Probably the largest cause.
2. **No CPU clock request**: `sb_acpm_dvfs` is called only for the GPU (`gpu.c`). Every core ran at S-Boot's
   ~1050 MHz.
3. **The M3 cores only on request** (the tier 3 PMU step); otherwise the A55s only.
4. **Latency-bound kernels:** INT and NEON are single dependency chains; FP is limited by divides, square
   roots and a serial sum.
5. The boot core works one batch per UI frame (minor).

(The 2.0 runs also "stopped at the temperature limit" while the sensors glitched, NOTES.md 2026-11-06: the
saved reports' run lengths and stop reasons are worth a look.)

**Fix, Engineer OS 2.2:** a **Heat** kernel (the same independent FMA streams) as a kernel choice and the
thermal test's default; a **Clocks** row on the CPU stress and thermal pages: "S-Boot's (about 1050 MHz)" or
"full: A55 1794 MHz, M3 1794 MHz" through the listed DVFS path (tier 3, as the GPU clock), the M3 domain
only while M3 cores run, restored to 1053 / 1066 MHz when the test ends, and the clocks the test really got
shown and logged.

## 9. First apps

| App | v0.1 | Later |
|---|---|---|
| **Light** | screen light: white, warm or red at full screen, off after 10 minutes or at a touch | the LED torch (9.1), 3 levels |
| **Heat** | Off / Low / Medium / High, time left, battery / skin / SoC temperatures, the state and why | the GPU in High |
| **SOS** | hold 1 s, 3 s countdown, then the screen flashes white / red at 2 Hz with a big "SOS" and the time; a hold stops it | the siren (audio codec), vibration (haptic PWM), Morse SOS on the torch |
| **Clock** | local time (Helsinki, the Utility OS's `tz.c`), world clock, stopwatch, timer | alarm clock with RTC power-on |
| **Battery** | %, mAh left, current, temperature, the estimates per state, learned or typical | history |
| **Settings** | screen-off time, interrupt mode, first-frame time, versions, clocks | persistent settings (v0.2) |

### 9.1 The flashlight LED (v0.2 plan, researched 2026-11-07, not yet run)

From LineageOS `android_kernel_samsung_exynos9810` (lineage-20, commit 6a9461f; GPL-2.0:
`drivers/leds/leds-s2mpb02.c`, `include/linux/leds-s2mpb02.h`, `include/linux/mfd/s2mpb02-private.h`,
`drivers/mfd/s2mpb02-core.c`) and the vendor DT. The S9's own torch (the Settings tile) is **switched by a GPIO**;
the S2MPB02 (I2C 0x59 on HSI2C 0x10920000) only sets the current.

| What | Where | Value |
|---|---|---|
| torch current | S2MPB02 FLED_CUR1 (0x14) bits 3:0, read-modify-write | n x 20 mA; the levels 2 / 4 / 7 (40, 80, 140 mA), at most 0x0C (240 mA) |
| torch on / off | torch1-gpio: gpg2 pin 4, PERIC0 `pinctrl@10430000` bank +0xC0 (CON 0x104300C0 bits 19:16 = 1 output, DAT 0x104300C4 bit 4) | high = on, low = off |
| flash pin | flash1-gpio: gpg3 pin 5 (0x108300C0): **kept low** (high = 1250 mA flash) | 0 |
| chip check | BST_CTRL2 (0x1D) reads 0x90 (rev 1) | read only |

Order: both pins outputs at 0 → read back 0x1D and 0x12..0x15 → CUR1 low nibble → torch pin high; off: pin low
first. Never written: FLED_CTRL2 (0x13) and 0x16..0x1B (the iris IR LED: an eye-safety risk), 0x63, the
camera rails (0x0B..0x11, 0x1E..0x32), the flash pin. Samsung's own driver writes INT1 (0x01) by mistake on the
off path (L140): not copied. Unknown without a datasheet: FLED_CTRL2's bits, whether the torch timeout applies
in GPIO mode, whether gpg3-5's other pin groups (hsi2c27, spi12, uart13) are in use. The first run is a phone
test with the user present (lowest level first, the read-back logged before any write).

## 10. Later (research and design first)

- **Modem (Shannon):** CP boot (firmware from the RADIO partition into the CP's reserved memory, the PMU's CP
  reset and the shared memory window), then Samsung IPC over shared memory: the vendor kernel's `modem_v1`
  (`link_device_shmem.c`, `modem_ctrl_ss310ap`), the userspace `cbd`, libsamsung-ipc for the formats. The
  modem's NV requests are served **read-only from EFS with writes going to a copy** in the OS store; nothing
  ever writes EFS. Calls, SMS, mobile data. The emergency number is implemented and never dialled in tests.
- **Wi-Fi:** the `wifi-core` session's portable BCM4361 core (track C).
- **Camera:** the front camera first (raw capture, debayer on the CPU; references: the Samsung kernel and the
  Galaxy Tab Active3 Linux port).
- **Audio:** the codec and the amplifier for the siren and calls. **Sensors:** track E's drivers.

## 11. Risks and what is unverified

| Item | Risk | Mitigation |
|---|---|---|
| GIC + CNTV delivery at EL1 | S-Boot's GIC state, groups | full reset, boot self test, polling fallback |
| EL0 apps | a wrong descriptor or register faults or exposes the kernel | the crash screen; a phone test with a deliberate fault and a busy loop |
| CPU DVFS under sustained load | sent before (iOS port), never under a power virus | exact levels, readback, ramp, current and voltage watch, the user present |
| M3 PMU defaults | worked in Engineer OS 2.1 and iOS | the same values, once per boot |
| Battery thermistor channel 4 | backed by the DT and one S-Boot reading | start cross-check, stuck detection, no fallback |
| The 115 °C trip | never seen firing | the kernel's own limits are far below it |
| Screen off with DECON running | no real saving in v0.1 | named honestly; v0.2 panel sleep |

## 12. Version 0.1

### 12.1 What is in it

Kernel: the event loop, timers, the GIC + CNTV with the self test and polling fallback, the watchdog, EL0
apps with the API, the `.sba` loader (built-ins packed into the image), the declarative UI renderer, input,
power (Active, Screen off, the reserve power-off), the battery model, the heater with its full safety logic,
SOS. Apps: Light, Heat, SOS, Clock, Battery, Settings. libsb: `sb_acpm_dvfs_listed`, `sb_m3_pmu_defaults`,
`sb_mmu_el0` and UXN on kernel blocks, two fonts' ids, icon ids up to 255.

Two things only for 0.1, for the first phone runs:
- **Boot self test of EL0** (interrupt mode only): two hidden apps start in the background. `test-loop` makes
  system calls, then spins forever: the budget must stop it ("test-loop stopped responding" in the log).
  `test-fault` reads the kernel image at 0x92080000: the read must fault ("test-fault: stopped (a fault, ESR
  ...)"); a log line "READ KERNEL MEMORY" would mean EL0 isolation is broken.
- **Dead-man return:** started and never touched (no touch, no key) for 10 minutes: the kernel restarts into
  the menu, so an unattended test boot always comes back.

| File | What |
|---|---|
| `kern/main.c` | the entry and the loop |
| `kern/timer.c` | the timer list (pure) |
| `kern/hw.c`, `kern/vec.S` | the phone's hardware layer: EL0 hardening, GIC, CNTV, sleep, the frame switch |
| `kern/app.c`, `kern/kapp.h` | loading apps, event queues, frames and the budget, the system calls |
| `kern/appfmt.c` | the `.sba` check and placement (pure) |
| `kern/ui.c`, `kern/input.c` | the renderer (lock, home, app screens, power menu, confirmations, light, SOS) and input |
| `kern/heatctl.c`, `kern/heater.c`, `kern/heatk.c` | the heater's controller (pure), its service, the heat kernel (ARMv8.0) |
| `kern/services.c`, `kern/batmodel.c` | SOS, the clock, the battery (and its model, pure), power |
| `kern/state.c` | the state slot format (pure; used from v0.2) |
| `sdk/` | `sbapp.h` (the API), `sbfmt.h`, the app runtime, the link script (MIT) |
| `apps/` | the built-in apps; `clock.c` links the Utility OS's `tz.c` |
| `tools/` | `sbos-assets.py` (fonts and icons, on the Mac), `mergeassets.py`, `mk-sba.py` (app header, pack) |

### 12.2 Host checks

- `starbite/tools/sbos-host-test.c`: timers, the app file check against the real built apps and broken copies,
  the dispatcher's pointer checks, the heater controller against scripted sensor traces (every hard stop,
  regulation, the cold start, the charger pause, stuck sensors), the DVFS list, the battery model, the state
  slot format.
- `starbite/host/sbos-preview.sh`: PNGs of every screen to `starbite/sbos/preview/` (lock, home, every app,
  the heater's confirmation and a running session, SOS countdown and flashing, the power menu, the key
  focus); every frame checked fully presented.
- The build: only R_AARCH64_RELATIVE relocations (kernel and apps), nothing indirect before relocation, the
  worker code ARMv8.0 only, no host paths, the relocation test, every packed app passes the kernel's check.

Results (2026-11-07): host test 315 checks (with the 8 built apps and their broken copies), 0 failed; the
preview's 19 scenes, 0 px unpresented; image 5181440 bytes, sha256 124cf3f6864b5e5de10edb351722716d730c95cc8d3ab747779a502d933af39f
(reproducible: two builds give the same bytes).

### 12.3 Engineer OS 2.2

Section 8.5's fix.

## 13. Design review (2026-11-07)

Three independent adversarial reviews (hardware and heater safety; kernel, EL0 and interrupts; power, storage
and UX) found 9 blockers and about 30 major points against the first draft. All are folded into the sections
above; the main changes:

- **Timer:** CNTV (INTID 27) instead of CNTP: Samsung's hypervisor could trap CNTP, the vendor kernel never
  armed it, XNU used CNTV here (2.2).
- **Apps as frames:** every exit from EL0 returns to the loop, so the heater and the watchdog never wait for an
  app; the budget is enforced by the timer armed before every `eret`; SCTLR / CNTKCTL / PMUSERENR written so an
  app cannot mask interrupts or the timer; UXN on all kernel memory; a full GIC reset; IRQs masked in the
  kernel (no lost wakeups); a stricter self test (one interrupt then silence, a timed `wfi`) (2.2, 2.3).
- **Heater:** no stale gauge-temperature fallback; exact DVFS levels with PLL readback; constant restore levels;
  domain 3 only with M3 cores on; M3 ramp; cold start; skin (AP) limit; step-down regulation; stuck-sensor
  checks; the charger read failing safe; hung workers caught with AFFINITY_INFO; current limits on the
  instantaneous current (8).
- **Engineer OS:** the memory kernel in the default mix is the main reason the stress test stayed cool (8.5).
- **Power and storage:** the state slot needs SF_VOLATILE, a range-checked fence, a standing consent and a
  cache flush before power-off; no per-cycle settings writes (the default entry instead); charger cases; real
  wake times (10-15 s from off); services never restored; the reserve keeps Light and SOS alive (4, 6.2).
- **UX:** Power is the screen key, Bixby selects; holds on the lock screen; a cancellable SOS countdown;
  emergency call behind hold + confirm and compiled out of test builds (3.1).

A fourth review read the built phone-only code (vec.S, hw.c, app.c, heater.c, the libsb changes): no blocker;
the frame layout, the EL0 entry and exit, the vector forwarding, the SCTLR bits, the GIC order and the pointer
checks verified. Fixed: the A55 clock left high when the M3 setup failed; apps could fill the timer list (now
24 entries reserved for the kernel); the app's FPCR leaking into the kernel (the heat kernel's reference);
an SError latched by the kernel blamed on an app (now taken by libsb before each `eret`); polling mode left the
GIC on; SCTLR's SPAN / WXN and PAN set explicitly; SPSel pinned first thing; the worker stop check with PSCI 0.1
(start timeouts, parked cores, self-stopped workers restarted); the app's entry copied at load (the header in
the slot is app-writable); a relocation offset overflow in the file check; the heap bounds; PXN on the slot
window at all times; TPIDR_EL0 kept across preemption.

## 14. Phone test requests

Starbite OS 0.1 writes no storage. Putting it on the phone means the usual store update by the main session:
`starbite-os.img` into the UFS store and the row of `starbite/sbos/catalog-entry.toml` into the catalog (it is
full at 32 rows: one hidden row has to make room, or `CAT_MAX` is raised in the loader), with the routine
backup first. Boot it by its catalog key, never by the file name.

**T1: first boot, unattended is fine** (about 11 minutes; no writes; comes back by itself)
- `sh tools/store/boot-next.sh starbite-os` (or `tools/store/sbfb.py next starbite-os` from the menu).
- Expect: the lock screen (big time, date, battery %) right after the loader's handoff. Then, untouched, after
  10 minutes the phone restarts into the menu by itself.
- In the pmsg ring afterwards (the loader's log, the Utility OS's `PMSG.TXT` or TWRP's pstore), in order:
  `Starbite OS 0.1`, `sbos: first frame at N ms since power-on`,
  `sbos: interrupts: GIC + virtual timer, wfi sleeps` (or `timer interrupt test failed` / `wfi returned
  early`: polling mode, still usable), `sbos: apps loaded: 8`,
  `app test-loop: test-loop: system calls work, now spinning`, `EL0 budget test stopped responding and was
  closed`, `EL0 fault test: stopped (a fault, ESR 0x92......)`, then `sbos: no touch or key for 10 minutes`.
- What could go wrong: a fault in the EL0 path shows libsb's crash screen; a hang is reset by the watchdog
  within 30 s; either way the loader's crash report has the log. `READ KERNEL MEMORY` in the log means EL0
  isolation is broken: report it, do not run T3.
- Undo: nothing to undo (no writes); the catalog row can stay.

**T2: by hand, 5 minutes, the user** — hold Open → home; Light → White → tap to end; Clock (world clock,
stopwatch); Battery (current and estimate); Settings ("Interrupts: on", the first-frame time, the clocks);
SOS: hold 1 s → 3 s countdown → flashing white / red → hold to stop; Power short → black, Power again → lock
screen; Power held 1 s → power menu → Boot menu (back to the Starbite menu).

**T3: the heater, the user present** (first run of CPU DVFS under sustained load)
- Heat → Low → "Use the heater?" → hold to allow. Watch the Heat screen 2 minutes (battery and processor
  temperatures, cores), then Off. Then Medium (writes the M3 PMU defaults, asks ACPM for the M3s at 1469
  then 1794 MHz), 2 minutes, Off; then High, 2 minutes, Off.
- Expect log lines `acpm dvfs domain 2 to 1794000 kHz (listed): answered`, `m3 pmu defaults: set`,
  `acpm dvfs domain 3 to 1469000 kHz (listed): answered`, and after Off the restores to 1066000 / 1053000.
- Risks: heat (the controller's limits: battery 40 °C regulation, 42 °C stop; SoC 75 °C regulation, 85 °C
  stop); a worker core that does not turn off makes the kernel restart the phone (by design). Undo: Off, or a
  restart (ACPM levels do not survive it).

**T4: Engineer OS 2.2, the user present** — CPU stress with Kernels "heat" and Clocks "full" (tier 3), A55
cores 2 minutes, then all 8: the page shows the clocks it got and the temperature; afterwards the log shows the
restore. Compare with a run at "as S-Boot left them".
