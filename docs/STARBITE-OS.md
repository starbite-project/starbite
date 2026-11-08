# Starbite OS

Track A of `docs/PLAN-2026-11-07.md`. Our own operating system for the Galaxy S9 (SM-G960F, Exynos 9810),
written from the lowest level up on libsb. Not Linux, not based on anything else. Ultra minimal and ultra
low power; it has what you want in an emergency without being called an emergency OS: a flashlight, a
heater, a loud alarm, a clock, honest battery estimates and, later, calls, texts and basic networking.

Status (2026-11-08, 0.5.12): design reviewed (section 13); first boot on S9 #1 passed (14.1); 0.2 phone run:
the LED works, the heater and the UI failed and were rewritten (0.2.1, 8.6); **0.3's unattended self test
passed on the phone** (T11, 14.5). Since then, host-built and host-tested only (406 host checks, the unit tests,
the settings, Wi-Fi and monkey tests, the 9-case soak, reproducible images), every step reviewed independently:
- 0.3: the real interface (3.1), the app framework ("Writing an app", 5.1), the self-test image (12.6).
- 0.3.2: settings saved on the phone through a fenced volatile store entry (6.5); screen-off low power and an
  optional switch-off (4.2).
- 0.4 .. 0.5.1: Wi-Fi on the merged BCM4361 core (9.4): open networks, the saved network, any WPA2 network typed
  on the new on-screen keyboard, DHCP, a router ping, the clock over NTP, the chip's interrupt.
- 0.5.2 .. 0.5.12: a quick Power press wakes the screen (an 0.1 bug), the medical card written on the phone and
  shown on the lock screen, the apps' states saved, charts (battery, heat), fewer wake-ups, review fixes.
- A short user guide: `docs/STARBITE-OS-GUIDE.md`. Phone tests waiting: 14.5 .. 14.7 (T14 .. T17).

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
- **Home (0.3):** a full-width red **Emergency** tile on top (call, message, alarm), then the apps in 2
  columns of large tiles (625 px wide, up to 600 px tall: an icon in a tinted disc, the name, and a live
  status line the app sets with `ui_tile`, e.g. "Off 26.2 °C", "76 % about 3 h"; the kernel adds "Heating",
  "LED on", "Alarm on" itself). A key hint line at the bottom.
- **Status bar (0.3):** the time on the left; a pill in the middle while a service runs (HEATING, SOS, LED
  ON); on the right the mobile signal and Wi-Fi glyphs (outlines only until their drivers exist), the battery
  % with the estimate ("3 h", "2 d", "45 min") and a battery glyph (green charging, orange under 20 %, red
  under 10 %). Redrawn only when its text changes.
- **Transitions (0.3):** a screen change between the lock screen, home and apps is presented in 4 vertical
  bands about 16 ms apart, from the right going in and from the left going back (one full draw, then 4
  partial presents: no extra drawing). Full-screen light and SOS appear at once.
- **Keyboard (0.5):** a screen that asks for text (`ui_input`) gets a field (dots for a secret, "Show" / "Hide")
  and an on-screen keyboard across the bottom 800 px: letters (shift: one capital), digits and signs ("123"),
  more signs ("#+="), space, Delete and the action key (Join). The text stays in the kernel until the app reads
  it, is wiped when the app leaves the front, and is never logged. Touch only (the keys do not type).
- **Style:** black background, cards #1c1c1e with 72 px corners, Inter / Inter Tight, white primary text and
  three greys, one accent per app (its tile colour), red only for emergency and danger, orange for warnings.
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
- v0.2 (built; **off until Settings turns it on**, `kern/panel.c`): **refresh only on change**: S-Boot itself
  leaves DECON_F's trigger masked (0x3070); the loader makes it free-running (0x1281). The kernel masks it
  (HW_TRIG_MASK_DECON, bit 4, as the vendor driver's `decon_reg_set_trigger`) while the screen is still, and
  every present unmasks it for 60 ms (two TE periods); **panel off with the screen**: the vendor's own
  display-off / display-on (level-1 key `9F A5 A5`, DCS 0x28 / 0x29, `9F 5A 5A`), never sleep-in (its way back
  needs the full panel init). DCS through DSIM as Engineer OS's brightness page sends it.
- Later: brightness through S-Boot's own AOR sequence (`F0 5A 5A`, `B1 hi lo`, `F7 03`, `F0 A5 A5`;
  build/em-research-brightness.md) with dimming after 15 s. Each is a phone test of its own.

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

0.3.2: with the screen off (and nothing heating) the A55 cluster drops to 455 MHz through the listed ACPM path
(PLL read back) and returns to 1053 MHz when the screen comes on; the heater takes the clocks over when it starts.
**Switch off when the screen has been off** (Settings > Display: Never (default), 5 min, 15 min, 1 h): when the
time is up and no charger, heat, alarm or LED needs the phone and the battery is above 3 %, every app gets
`EV_APP_SAVE`, the settings are written (FUA + flush) and the phone powers off; otherwise it asks again in 10
minutes. Waking is the power key through S-Boot and the loader (Starbite OS as the loader's default entry). The
self-test image rehearses it (everything but the power-off). Real RAM sleep (4.3) stays a research item: it needs
the SoC's own suspend path and a phone test with the user present.

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
| 10..29 | UI | `ui_input(id, label, flags, max)` (24, 0.5: a text field and the on-screen keyboard), `ui_input_get(buf, cap)` (25), `ui_begin`, `ui_title`, `ui_button(id, label, sub, flags)`, `ui_big(caption, value, colour)`, `ui_textline`, `ui_level(id, "Off|Low|...", current)`, `ui_toggle`, `ui_hold(id, label, ms, flags)`, `ui_end`, `ui_toast`, `ui_light(argb)` (full-screen colour), `ui_awake`; 0.3: `ui_tile(line)` (22, the home tile's status line, any time), `ui_gauge(caption, value, permille, colour)` (23, a ring) |
| 30..39 | time | `time_now(struct sbos_time*)`, `timer_set(id, ms, repeat)`, `timer_cancel`, `uptime_ms` |
| 40..49 | power | `battery`, `estimate`, `power_request`, `sysinfo`, `setting_get` / `setting_set` (0.2; 0.3 adds `SET_BRIGHTNESS`: 0 as the boot left it, 1..4 low..high) |
| 50..59 | light | `torch(level)` (`SBOS_ENOSYS` until the LED driver exists), `strobe` |
| 60..69 | heat | `heat_request(level, minutes)`, `heat_status` |
| 70..79 | alarm | `sos(on)`, `vibrate`, `alarm_set`, `alarm_cancel`, `sos_status` |
| 80..89 | state | `state_put`, `state_get` (4 KiB per app) |
| 90..99 | files | read-only card files (reserved) |
| 100..119 | sensors | 0.3: `sensor_read(id)` (100): battery, skin, SoC, GPU temperature in 0.1 °C, -1000 unknown (`SBOS_P_SENSORS` or `SBOS_P_HEAT`) |
| 120..139 | phone | `phone_state`, `call_dial`, `call_hangup`, `sms_send`, `sms_read` (reserved) |
| 140..159 | network | reserved |
| 160..179 | camera | reserved |

Events: `EV_APP_START`, `SHOW`, `HIDE`, `BUTTON(id)`, `LEVEL(id, value)`, `TOGGLE(id, on)`, `HOLD(id)`,
`TIMER(id)`, `BACK`, `SAVE`, `POWER(state)`, `SERVICE(id)`.

### 5.1 Writing an app (0.3)

An app is one C file and one manifest line. `starbite/sbos/apps/template/hello.c` is a complete example.

1. **The manifest:** `SBAPP_DEFINE("id", "Name", permissions, tile colour, icon)` at the top of the file
   becomes the app's 256-byte header (6.1): a unique id, the tile's name, the `SBOS_P_*` permissions it needs
   (a call outside them answers `SBOS_EPERM`), the tile's accent colour and one of the system's icons.
2. **The entry point:** `void app_event(const struct sbos_event *ev)`. `EV_APP_START` comes once after
   loading (set the tile line, start a slow timer), `EV_APP_SHOW` each time the app comes to the front:
   describe the screen between `ui_begin()` and `ui_end()`. Taps arrive as `EV_APP_BUTTON` / `LEVEL` /
   `TOGGLE` / `HOLD` with the element's id; describe the screen again after a change (the kernel redraws only
   what differs). Back always returns home (the app gets `EV_APP_BACK`, then `EV_APP_HIDE`).
3. **The screen:** elements flow from the top: a title, buttons (`UI_HALF` puts two in a row), big values,
   text lines (up to 3 lines each), level pickers (also good as section tabs: Settings), toggles, hold
   buttons, a gauge. A screen taller than the panel is logged ("ui: app screen taller than the panel") and
   fails the self test: split it into sections.
4. **Build:** add the id to the app list in `starbite/sbos/build.sh` (and for the host preview to
   `starbite/host/sbos-preview.sh` and `sbos-host-stubs.c`). `mk-sba.py` checks and packs it; the kernel
   checks it again at load (CRC, sizes, relocations).

Each handler has 500 ms; one that faults or runs over is stopped (its tile stays, it gets no more events).

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

### 6.2 State slot (v0.3; was planned for 0.2)

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

### 6.5 The state slot on the phone (0.3.2)

- **The entry:** `sbos-state`, 1 MiB, the directory's volatile flag (like the loader's `crashlog`), made on the Mac
  at a store rebuild: `tools/store/sbstore2.py build ... --volatile sbos-state=1048576` (zeros when new; `--import`
  of the live store keeps its bytes and flag; tested in `tools/loader-test/test_sbstore2.py`). Its existence is the
  install decision and the consent: without it Starbite OS writes nothing anywhere.
- **libsb:** `store_volatile_grant` (store.c) finds the entry on UFS, checks the volatile flag and the size, and asks
  the fence for a standing grant of exactly its blocks (`sb_volatile_grant`, fence.c: UFS only, inside the live
  store's data area, never blocks 0..6 of the store (header, settings, directory), at most 1024 blocks).
  `sb_write_volatile` writes only inside the grant, logs every write (`fence: wrote 1 blk at ufs:N (sbos-state,
  tier 1): FUA + cache flush`), honours the dry run, and goes through `sb_blk_write_sync`: WRITE(10) with FUA,
  then SYNCHRONIZE CACHE(10). The grant is not a gate authorisation: `sb_write` and `sb_write_store` still refuse
  the same blocks (unit-tested: 546 checks).
- **The kernel** (`kern/slot.c`): halves at blocks 0..127 and 128..255 of the entry, read after the first frame
  (UFS on S-Boot's running host), every write read back and compared.
- **The self-test image** (12.6): where the entry exists, a dry run of a save (logged only), then one real save,
  both halves read again and the newer picked: `selftest: slot: ... read back and picked: ok`.

### 6.4 Settings across a restart (0.3.1: the kernel side)

`kern/settings.c` keeps the record `sbos.settings` (version 1: the screen-off time, refresh only on change, the
panel off with the screen, the LED in the alarm, the brightness) in the state slot of 6.2. A change is saved
3 s after the last one, into the half that does not hold the newest valid copy (a damaged write leaves the
older copy); the start restores it after `power_init`. Never saved: the GPU in High and the heater's first-run
confirmation (asked again every start). The slot is behind `hw_slot_read` / `hw_slot_write`: the host preview
keeps it in a file (`SBOS_HOST_STATE`, tested by `starbite/host/sbos-settings-test.sh`: save, restore, a damaged
newest half, no slot); **on the phone there is no slot yet**: steps 1 to 4 of 6.3 (the `sbos-state` entry, the
fence's range check, the volatile writer, UFS with FUA and a cache flush) come first, and the kernel does not
bring UFS up today. Until then the phone logs `settings: no state slot yet` and Settings says the settings last
until a restart.

### 6.3 Save and power off: the implementation plan (v0.3)

The order, each step reviewed and host-tested before the next, the phone only with a store backup:
1. **The entry** (the Mac, an install step): `sbstore2.py` gets `sbos-state` (256 blocks, directory flag 1 =
   volatile, like `crashlog`), added to the UFS store by the main session. Its existence is the user's install
   decision; Starbite OS never creates or grows entries.
2. **libsb `fence.c`**: `sb_write_store` checks the open authorisation's own range (first / count), not only
   "inside the store" (a review finding: today the header, directory and settings would pass).
3. **libsb `store.c`**: `store_volatile_write(medium, name, block, src, n)`: only an entry with the volatile flag,
   only blocks inside it, through `sb_write_store`, read back.
4. **libsb `ufs.c`**: WRITE(10) with FUA for these blocks, then SYNCHRONIZE CACHE(10) and START STOP UNIT
   (power condition: sleep) before `sb_power_off`, so nothing waits in the device's cache when the power goes.
5. **The kernel**: a Settings switch "Save and switch off after 5 minutes" (the standing consent, itself saved in
   the slot; off on a fresh slot), `EV_APP_SAVE`, A/B halves (`state.c`, already host-tested), the restore at
   start (never services; skipped after an unclean reset or with Volume Down held).
6. **Waking**: the user makes Starbite OS the loader's default entry with the quick POST once (a loader setting);
   the kernel writes nothing to the settings block.
7. Phone: a dry run (`sb_fence_dry_run`) that logs every block it would write, then one real save with the
   blocks read back on the Mac, then the power-off cycle.

## 7. Boot speed

Order: libsb's start, `ui_init` (fonts, the 17 MB back buffer cleared), the lock screen drawn and presented,
*then* the GIC and the timer test, touch, the gauge, the built-in apps. The time zone is the home zone
(Helsinki) until the state slot exists. The kernel logs `sbos: first frame at N ms` (counted from power-on
by the counter, so it includes S-Boot and the loader); from 0.3.1 the same line splits it: when Starbite OS
started, then fonts and back buffer, the battery read and the first draw; `sbos: ready ... at N ms` follows
once interrupts, touch and the apps are up.

The 0.3 self test on the phone (T11) had its first frame at 12508 ms after power-on. Most of that is before
Starbite OS: the loader's **full POST took 7514 ms** in the same log (`STARBITE: POST full 7514ms`), on top of
S-Boot. The loader's quick POST takes about 0.3 s (docs/STARBITE-HEALTH.md 2.1: a loader setting, Bixby at boot
→ STARTUP SETTINGS → POST AT BOOT, or `sbstore2.py settings ... --post quick`); that one setting would bring the
first frame to about 5 s. It is the user's loader setting, not Starbite OS's to change. The image is about 5 MiB (4.3 MiB of it fonts and
icons) for the loader to read.

## 8. Heater

The phone deliberately makes heat (hand warmer, keeping itself above freezing). A kernel service; the Heat
app only asks for a level and shows the status.

### 8.1 How it makes heat

| Level | Load (since 0.2.1; cpu0 works between the kernel's events instead of sleeping) |
|---|---|
| Low | cpu0-3 (all four A55) at 1794 MHz (ACPM domain 2) |
| Medium | Low + cpu4-5 (M3) at 1469 MHz, then 1794 MHz after 60 s if the battery voltage holds (domain 3) |
| High | all 8 cores, the same ramp; + the GPU at 572 MHz (domain 4) when Settings allows it (8.6) |

- **Heat kernel** (`kern/heatk.c`, ARMv8.0), since 0.2.1 **non-repeating and compute-bound**: each core owns a
  4 MiB buffer. Normal batches work inside a 256 KiB window of it (held by the caches): every 64 bytes are
  converted and scaled into a bounded range (no NaN, no denormals), feed twelve NEON FMA streams for 16 rounds
  (192 FMAs per 64 bytes: an M3's three FMA pipes stay full) and are written back changed: an xorshift step plus a seed
  that differs per batch and per core (the xorshift alone is linear over GF(2) and came back to the same data
  after 4 passes; a host test caught it). Every 16th batch streams 512 KiB from the rest of the buffer (DRAM: the
  memory system's share) and runs the fixed reference block, compared with the boot core's result (a wrong result
  stops the heater: an unstable core). A first version streamed everything from DRAM: the review pointed out
  that eight cores would then mostly wait on memory, the very thing that kept Engineer OS cool (8.5).
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

### 8.6 Worker cores never block the kernel (0.2.1, after the 0.2 phone run)

0.2 started workers with libsb's `sb_smp_start`, which waits up to 1 s for a core's check-in. On the phone a
core never checked in (its PSCI state left over from the earlier Medium / High run), and the heater retried it on
every 4 Hz tick: the kernel's single loop was blocked about 1 s per tick, the running workers' dead man's switch
(1 s) fired, the Heat app's events piled up ("event queue full"), the screen-off timer, the wake and the power
menu starved. Reproduced on the Mac (14.3) and fixed:
- libsb `sb_smp_launch`: one PSCI CPU_ON, never a wait; the answer read from w0 (ALREADY_ON / ON_PENDING are
  recognised now; the old 64-bit compare never matched them).
- The heater runs a state machine per core: launched → (check-in within 1 s) running, or failed. A failure is
  retried after 2 s, then 4 s; the third disables that core until a restart. Each failure is logged once with its
  reason and code; each core that starts is logged (`heater: core 5 (M3) running, 6 cores with cpu0`). When no
  core of the level can run, the heater stops and the Heat screen says so ("The processor cores did not start
  (code -100). A restart fixes it.").
- A stop only signals the workers and returns (`sb_smp_stop(0)`); the stop check on the next ticks confirms
  every core left within 1.5 s. (Waiting there blocked the loop up to 500 ms on the phone; the soak caught it.)
- At most one CPU_ON per tick, and none while a core has not yet checked in. **Every core has its own start
  record** (libsb `sb_worker_rec`): CPU_ON passes PSCI's context id `0x5B5B5B00 | core`, and the worker entry
  (`crt0.S`) uses the core's own record when that id and the record's magic check out, else the old shared
  record. The first check-in tells which the firmware does (`heater: the firmware passes / drops PSCI's
  context id`). With shared records a core that boots late could take a newer core's record and stack, so then,
  after a check-in timeout, no further core starts in that heater session (the running ones go on), nor in later
  sessions until a restart. Every running core's slot identity (the MPIDR it wrote at check-in) is checked on
  every tick: a second core writing the same slot (a late core on a shared record) stops the heater; this needs
  no assumption about the MPIDR's format. A refused CPU_ON leaves the slot as it was (its PARKED marker). A core whose last CPU_OFF returned (parked) while PSCI calls it on is unusable until a restart, at
  once. Failures reset at a new session; a give-up never leaves a raised clock behind. The dead man's switch
  at 2 s; every loop turn of 200 ms or more is logged
  (`sbos: a loop turn took N ms`).
- App events that describe a state (a timer, a service, the power) are coalesced (one of a kind in the queue,
  updated in place) and take at most half of an app's queue: a busy service can no longer fill it; "queue full"
  is logged at most every 10 s.
- The charger reading changes only when two reads in a row agree, and every change is logged with the raw
  CHG_INT_OK byte and the current (to settle "paused although unplugged").

**The GPU's share on High** (`kern/gpuheat.c`): libsb's Mali job path has run on the host's fake Mali only
(Engineer OS 2.1's GPU tests are not yet run on the phone), and it needs vdd_g3d (BUCK6) at 950 mV, a PMIC write.
So it is **off until Settings → "GPU in Heat High (test)" is confirmed** (a warning and a hold, which also form
the write fence's authorisation for that one register). Then on High: the G3D domain must already be on, GPU_ID
a Mali-G72, PLL_G3D locked, TMU_SUB's trip armed, BUCK6 in its known range → 950 mV read back, libsb's GPU
session, reset and cores, ACPM domain 4 at 572 MHz; chains of clear-only fragment jobs over a 1024 x 1024 target on
job slot 0, resubmitted from the 4 Hz tick (never waited for), doubled until one chain lasts 200 ms, a few pixels
checked after each; the GPU's TMU zone counts in the SoC limits. Any failure or fault: the GPU's share stops for
the session (logged), the CPU load goes on; an unreadable GPU probe or the Settings switch turned off stop it
too. Every stop: job slot 0 hard-stopped and waited for (5 ms at most), a GPU soft reset, the cores off, domain 4
to 260 MHz, then BUCK6 back (also after a failure half-way through the bring-up). The GPU's share starts once
three workers run on High and stops only when the load stops (holding, paused, a stop), so the regulation's
core-by-core steps do not switch vdd_g3d up and down.

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
| **Light** | screen light: white, warm or red at full screen, off after 10 minutes or at a touch; **0.2: the LED** (9.1), on / off, off after 10 minutes | 3 LED levels (the S2MPB02's current over I2C) |
| **Info** (0.2) | the emergency card: `/starbite/emergency.txt` on the card (read only), the first line big (the name), one fact per line; an example in `starbite/sbos/examples/` | a lock-screen shortcut |
| **Heat** | Off / Low / Medium / High, time left, battery / skin / SoC temperatures, the state and why | the GPU in High |
| **SOS** | hold 1 s, 3 s countdown, then the screen flashes white / red at 2 Hz with a big "SOS" and the time; a hold stops it | the siren (audio codec), vibration (haptic PWM), Morse SOS on the torch |
| **Clock** | local time (Helsinki, the Utility OS's `tz.c`), world clock, stopwatch, timer | alarm clock with RTC power-on |
| **Battery** | %, mAh left, current, temperature, the estimates per state, learned or typical | history |
| **Settings** | screen-off time, interrupt mode, first-frame time, versions, clocks | persistent settings (v0.2) |

0.3 screens: **Light** the LED on/off (a GPIO: no current levels until 9.2), the screen light at Low / Half /
Full in white, warm or red. **Heat** Off / Low / High, the battery temperature big (with the 42 °C stop while
running), the processor's and the cores, time left, why it stopped, an orange warning (hot to hold, battery use).
**SOS** is called "Alarm" on its screen: flash, the LED strobe if Settings allows, "sound: not available yet".
**Battery** a ring gauge with the %, the estimate big, screen-off and details. **Settings** in four sections:
Display (brightness, screen-off time, refresh only on change, panel off), Alarm (the LED strobe), Heat (the GPU
in High), About. **Emergency** (new, 9.3).

### 9.4 Wi-Fi (0.4: open networks)

The BCM4361 through the wifi-core session's portable core (`starbite/wifi`: bcmw, merged from
`claude/wifi-core-c56c49`, docs/WIFI-BCM4361.md) and its IPv4 layer (sbnet: DHCP, ARP, ping, DNS), in
`kern/wifi.c`, with the app **Wi-Fi** (id `wifi`, `SBOS_P_NET`):
- The radio is off until the user turns it on. The bring-up runs one step per tick (the longest, the power
  sequence, about 250 ms), then a scan; the networks are listed strongest first (open ones can be joined,
  secured ones are shown with "Secured (later)"); a join is the non-blocking `bcmw_connect`, then DHCP, then
  one ping of the router ("The router answers in N ms"). The status bar's Wi-Fi glyph lights when joined.
- **Heat and Wi-Fi do not run together in 0.4**: the core's commands can wait up to 2 s for the chip, and the
  heater's 250 ms safety ticks must never wait behind them; each refuses while the other is on.
- Polling (every 20 ms with the screen on, 200 ms off); the chip's interrupt (SPI 253) later. Off, a restart and
  the power-off hold the radio in reset and unpowered (`bcmw_quiesce`).
- Writes only what `bcmw_write_allowed()` lists; the radio's files are read from VENDOR (read only). The pmsg ring
  gets states, counts and times only: no network names, no addresses.
- API: `SYS_NET_STATE` 140 (`struct sbos_wifi`), `SYS_WIFI_POWER` 141, `SYS_WIFI_SCAN` 142, `SYS_WIFI_LIST` 143,
  `SYS_WIFI_JOIN` 144.
- Host: the real core runs against the core's own register-level model (`starbite/wifi/host/model.c`), which gained
  an open-network mode for Starbite OS's preview (`M.open_ap`: no RSN, DHCP, ARP and ping answered; the core's own
  test keeps WPA2 and passes, 461 checks). Preview scenes `wifi`, `wifi-list`, `wifi-joined`.
- The self-test image turns Wi-Fi on, waits for the bring-up and one scan, logs the count (`selftest: wifi:
  networks heard: N`), and turns it off; it never joins.
- Review (independent, of the glue): fixed before the phone request: the core's DMA blocks are reused across
  on / off (the n-th request of a power-on gets the n-th block of the first; logged `wifi: DMA blocks: N new, M
  reused`; host: 20 cycles, then a join); quiesce only after a successful init; DHCP starts from the tick, not
  inside the event callback; the core's step errors reach the log (only the firmware's console is left out);
  late scan results never change the list shown; a dead chip or a lost lease is noticed; FAILED does not block
  the heater; the heater checks Wi-Fi again when its confirmation completes and Wi-Fi refuses while it is up; the
  self test starts the radio from off and a touch during it turns the radio off.

### 9.3 Emergency (0.3: screens only)

The red tile at the top of home. **Call 112** is a hold (1.2 s); in 0.3 it opens a page saying no call was
made: there is no modem driver, and the app contains no call or message request at all (no `SYS_CALL_DIAL`,
no `SYS_SMS_SEND`; the kernel answers `SBOS_ENOSYS` to them anyway). **Message** offers three fixed texts
(need help / safe / hurt, cannot talk) and says "not sent". **Hold for the alarm** starts SOS. When the modem
work starts (doc 10), dialling goes behind its own hold and confirmation, and is never part of a test.

### 9.1 The flashlight LED (0.2: the GPIO part is built, `kern/torch.c`; not yet run on the phone)

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

0.2 switches **the GPIO only**: the S2MPB02's I2C bus (USI at 0x10920000) was not brought up by S-Boot, and
bringing it up means CMU gate, SYSREG USI-mode and pin-mux writes (a step of its own, later); the torch channel
cannot exceed 300 mA whatever the chip holds. Before the first switch it reads both banks (a probe) and logs
them, and refuses when they do not read or when the flash pin is an output driven high; the flash pin is never
written. The SOS screen can flash the LED too (Settings, off by default). The full order with the current set
(later): both pins outputs at 0 → read back 0x1D and 0x12..0x15 → CUR1 low nibble → torch pin high; off: pin
low first. Never written: FLED_CTRL2 (0x13) and 0x16..0x1B (the iris IR LED: an eye-safety risk), 0x63, the
camera rails (0x0B..0x11, 0x1E..0x32), the flash pin. Samsung's own driver writes INT1 (0x01) by mistake on the
off path (L140): not copied. Unknown without a datasheet: FLED_CTRL2's bits, whether the torch timeout applies
in GPIO mode, whether gpg3-5's other pin groups (hsi2c27, spi12, uart13) are in use. The first run is a phone
test with the user present (lowest level first, the read-back logged before any write).

### 9.2 A brighter "boost" light (design, 2026-11-07; needs the S2MPB02's I2C first)

What the LED driver allows (leds-s2mpb02.c / .h, 9.1): the **torch** channel at 20..300 mA (FLED_CUR1 bits 3:0,
n x 20 mA) for continuous light, and the **flash** channel at 150..1550 mA (bits 7:4, 50 + 100 n mA) with a
hardware timer FLED_TIME1 bits 7:4 of 62.5 x (n + 1) ms that cuts the flash off by itself. Samsung's own torch
never exceeds 180 mA (its torch table), its camera flash is 1250 mA for 500 ms.

0.2 cannot do either safely: it switches the torch pin only, at whatever current the chip holds. Driving the
flash pin without having set the flash current and its timer over I2C would mean an unknown current for an
unknown time, so the flash pin stays untouched until the I2C bring-up (USI 0x10920000: CMU gate, SYSREG USI mode,
pin-mux; a reviewed step with its own phone test) has written and read back CUR1 and TIME1.

The choice, once I2C works:
- **Bright** (continuous light): the torch channel at **300 mA**, the chip's own ceiling for continuous light,
  1.7x Samsung's brightest torch (180 mA). Because the LED and the camera module heat up, Bright lasts **2 minutes**,
  then steps down to 180 mA for a **3-minute cooldown** before Bright can be used again; it stops early when the AP
  thermistor (the back) reaches 43 C, the battery is below 15 % or VCELL below 3.5 V. A brighter steady light is
  not offered: the flash channel is not rated for continuous current, and a pulsed "steady" light would flicker.
- **Beacon** (to be found, not to light the way): the flash channel at **1000 mA** for **100 ms once a second**
  (10 % duty: 100 mA on average, a third of the torch's ceiling), with the hardware timer set to **125 ms** (n = 1),
  so a stuck pin or a hung kernel can never hold more than one 125 ms pulse. Up to 10 minutes per session, then a
  2-minute rest; the same temperature and battery stops; never at the same time as the torch. 1000 mA rather than
  the camera's 1250 mA: a margin below Samsung's own setting, still about 3x the torch's brightness per pulse.
- Both through the Light app's buttons and SOS (the beacon pattern doubles as SOS's light), the current read back
  over I2C before the pin is driven and logged.

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

### 12.5 Version 0.2.1: the 0.2 phone failure fixed (2026-11-07)

The heater and the UI failed on the phone in 0.2 (14.3). Each bug was reproduced on the Mac before it was fixed,
the fixes were reviewed by an independent adversarial review (11 findings, all fixed: 8.6), and the whole UI
was soaked in the host preview:
- **Reproduction**: `host.c` gained PSCI failure injection (`SBOS_HOST_SMP=timeout:1,already_on:2,fault:3,
  parked:1,m3`). The 0.2 kernel with `timeout:1` shows the phone's log line for line ("core did not start,
  worker 1" once a second, cores 0), only 42 frames in 260 s and a power menu that never reboots.
- **The soak** (`starbite/host/sbos-soak.sh`): about 2 minutes of fake time per case, driven by keys and holds:
  the heater Low → High, every Settings switch, the LED and the screen light, the stopwatch, Battery, Info, SOS
  start and cancel, the power menu (Cancel), screen off / on, the heater on again, then the power menu's Boot
  menu with the heater running. It passes only with a reboot from the power menu while heating, no crash, every
  frame presented, no "event queue full", no loop turn of 200 ms or more, no A55 clock left raised, and each
  case's own lines. Cases: clean, all8 (the M3s available: `core 7 (M3) running, 8 cores with cpu0`),
  timeout1 (the phone's failure: the other cores start around it), timeout1noctx (the firmware drops PSCI's
  context id: launches held), alreadyon2, fault3, parked1 (Off, then a relaunch refused at once as parked),
  allfail (the give-up message), gpu (the GPU's share confirmed in Settings, 572 MHz, chains, stopped cleanly).
- Host fakes made honest where they hid or invented problems: the thermistors follow the thermal model (a raw
  value that never moved tripped the heater's stuck-sensor stop), the fake Mali runs chains on a thread (it had
  cleared 1 GiB inside one register write), the fake ACPM moves the fake GPU PLL.
- Image `starbite/out/starbite-os.img` 0.2.1: 5222400 bytes, sha256
  1b7e962a63fad4381a66e114134ef3b52b18da16e52d8d5a1e02f908cbc71c07, reproducible.
- `sbos-host-test`: 358 checks (new: the heat batch stays inside its buffer, never repeats, reports a wrong
  reference; the GPU's DVFS levels; the stop-check decisions).
- **A second independent review** checked the 11 fixes (10 verified, the start-record race only half closed) and
  the new per-core start records in `crt0.S` (correct: registers, the magic, the MMU-off store and its visibility,
  the fallback); its remaining points are fixed: the slot-identity check, launches held across sessions with shared
  records after a missing core, the GPU starting on running (not requested) workers, `buck_restore` never
  claiming success after a failed read, `sb_smp_start` retrying only the CPU_ON, a refused CPU_ON keeping the
  slot, 12 FMA streams (an M3 has three FMA pipes of about 4 cycles' latency).

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

Results (2026-11-07): host test 326 checks (with the 8 built apps and their broken copies), 0 failed; the
preview's 20 scenes, 0 px unpresented; image 5181440 bytes, sha256 b3aad7624ea405c9c99dc6d04ed3ba1f872de96c37a3c3e64292dca36f32a958 (0.1.1)
(reproducible: two builds give the same bytes).

### 12.6 Version 0.3: the interface (2026-11-07)

- The look (3.1): the status bar, the Emergency tile, live tile lines, the band transitions; the app screens
  of 9; `ui_tile`, `ui_gauge`, `sensor_read`, `SET_BRIGHTNESS` (S-Boot's AOR command, as Engineer OS sends it;
  nothing is sent until a level is picked in Settings); "Writing an app" (5.1) and the template.
- **The self test** (`kern/uitest.c`): only in `starbite-os-selftest.img` (build.sh links `uitest.c` a
  second time with `-DSBOS_SELFTEST_ON`; the normal image has `sbos_selftest_enabled = 0`). 5 s after the start
  it opens the lock screen, home, every app and a few of their pages (Emergency's message and Call pages,
  Light's strengths, the world clock, Settings' sections), the power menu and the screen light, one step every
  0.7 s, and logs to pmsg one line per screen: `selftest: app heat: app, 7 elements, slowest draw 4 ms, app
  ok, fits, handler 0 ms so far`; then `selftest: passed: ...` or `selftest: FAILED: ...` (an app dead, a
  screen empty or too tall) with the slowest draw and loop turn, and a restart to the loader's menu after 3 s.
  It never touches the LED, the heater, the alarm, the brightness or panel settings, and never calls or sends;
  any touch or key stops it. On the host: `SBOS_HOST_SELFTEST=1`.
- Host: `sbos-preview.sh` 31 scenes; `sbos-contact.py` makes one contact sheet of them; the soak's key paths
  follow the new home (Emergency first) and Settings' sections, and it fails if the brightness was ever sent.
- Review (an independent read of the 0.3 diff, no blocker), fixed before the phone request: in the self-test
  image a stray touch no longer turns off the 10-minute return to the menu (it counts from the last input);
  Heat shows the temperature now, not the last session's; Brightness "Boot" after a change sends S-Boot's
  normal AOR (0x0440); no transition when the screen wakes; the overflow log's bit shift; long tile lines cut
  on a character boundary before the compare; build.sh fails if the self-test image did not get the self test.

### 12.16 Version 0.6.3: the heat record, Manual's SoC stop, the LED at its rated currents (2026-11-08)

From the user's 0.6.2 tour: about 10 W at most, and "at 128 °C it force shut off" (the SoC reading on screen; the
loader then logged a POWER ON reset, so the RAM log was lost). The chip's own power cut acted near 128 °C on our
reading, not at the 115 °C we decode from THD_TEMP0 (either the decode or our probe conversion differs; noted, not
yet explained).
- **The heat record** (`struct heat_record`, the slot's "heat" record): the level, time, cores, the last and the peak
  temperatures, current, voltage, charge, the decoded trip and the reason, saved every 30 s while heating (through
  the settings' save: at most one write in 10 s) and at the stop. A record still marked "running" at the next start
  is logged (`heater: the last session ended without a stop (a forced power-off?) ...`) and shown on the Heat screen.
  Needs the `sbos-state` store entry (T14's install step); without it nothing is written.
- The Heat screen shows the chip's power-cut temperature as decoded, with "it acted at 128 °C in a test".
- **Manual's SoC stop** (`SET_HEAT_SOC_STOP_C10`): off by default (the user's wish), or 90 / 100 / 110 / 120 °C.
- **The LED at its rated currents** (`kern/fled.c`, Settings > Alarm "LED at rated power", off by default and until a
  restart: a first run with the user present): the S2MPB02 over HSI2C 0x10920000 (USI10) at 0x59
  (`build/em-research-flash-led.md`). Probes 0x1D = 0x90 and 0x63 = 0x7F first, reads the USI10 gates (never
  writes CMU), writes the USI mode and gpp6-0/1 only if they differ, and of the chip only CTRL1 bits 7:6 / 3, CUR1
  and TIME1[7:4]. The torch at 240 mA (Samsung's highest continuous), the alarm's LED as 1250 mA flash pulses (the
  camera flash) ended by the chip's 125 ms timer, at most one a second, never with the torch pin high. The host has a
  model of the chip that flags a forbidden register.

### 12.15 Version 0.6.2: emergency heat without software limits (2026-11-08)

The user's scenario (Lapland, -35 °C, the phone is all they have), agreed with the main session:
- **Extreme, Manual and Keep warm have no software limits** but the battery's **55 °C** (its fire safety), the
  readings that stop needs (a lost or stuck battery/SoC reading), a core fault, the 3.5 A battery current (on by
  default; Manual has a hold to turn it off, the user's request), the SoC's hardware trip reading unarmed (it is the
  protection now: S-Boot arms it at 115 °C, libsb only reads the TMU, and the heater refuses to start without it; the
  GPU's zone too while the GPU runs), and a **call / SOS reserve**
  (5 / 15 / 30 %, default 15; a hold on "Hold to go on below the reserve" overrides it until a restart). Gone for
  these levels: our SoC stop (S-Boot's 115 °C hardware trip stays armed), the 3.45 V floor (the PMIC's and the fuel
  gauge's own protections stay), the 40 % start minimum. Starts below 50 °C battery.
- **Keep the battery warm** (`HEAT_KEEPWARM`): a battery target (10 / 20 / 30 °C): every core while the battery is
  more than 0.5 °C below it, one off per second above +0.5 °C; the cores alone (no GPU). In deep cold a warm cell
  keeps its capacity and its current.
- **The estimate first**: each confirmation says about how many minutes of full heat the charge above the reserve
  gives at 1.9 A (the 0.6 tour's draw). While heating: the measured current and power (A, W) and the minutes left to
  the reserve (3000 mAh). One line explains that the heat is the battery's power (~7 W), not the clocks.
- **Clock steps from the chip's own ECT** (`build/em-research-g3d-ect.md`, read from S-Boot's ECT; the VG4 boot-LUN
  backup holds the same bytes): the A55's cap is **1794 MHz** (GEN `MINMAX_dvfs_cpucl0`, every ASV version), so
  0.6.1's 1950 MHz for Extreme was above Samsung's own limit: Extreme is back at 1794. Manual offers 1898 / 1950 MHz
  (in the table with the ECT's own ASV voltages) with a warning, as the main session decided (Windows runs its A55s
  at 1950 since 2026-11-07); 2002 MHz never. The GPU's ECT top is **598 MHz** with its own ASV voltages (inside `MINMAX_dvfs_g3d`; Android's
  DT caps it at 572): offered in Manual. No 650 / 702 MHz level exists in this chip's ECT.
- **The live readout** on the Heat screen while heating, every second: the battery current (mA) and power (W, V x I
  from the fuel gauge), the minutes to the reserve, the temperatures (battery, SoC, the back).
- **The 3.5 A stop** stays on by default (it never limited heat: the tour peaked at 1.8 A); Manual has a hold to turn
  it off (`SET_HEAT_CURRENT_STOP`, until a restart), the user's request.
- **App pages scroll** (the user: the Manual page's bottom was cut off): a vertical drag scrolls a page taller than
  the panel (Back stays; the title and content move under it, clipped below y 300; a scroll bar); the key focus
  scrolls to its element. The self test logs "scrolls N px" per page and counts a page that lost elements (more
  than fit) as "too tall"; it now also opens Heat's Manual page (30 screens).
- Wi-Fi: wifi-core's e14fca2 (`join_pm_off`, the log of a handshake frame from another address) folded into the
  split join; `sbos-wifi-test.sh` waits for the polled join's 15 s timeout (it failed on 0.6.1 for that reason
  only) and checks that the loop never blocks during it.

### 12.14 Version 0.6.1: Extreme flat out, Manual heat, WPA2, Emergency Mode's power profile (2026-11-08)

From the user's tour of 0.6 (`build/starbite-os/tour06/pmsg.txt`):
- **Extreme was a thermostat by design** (the user: it shed 8 → 4 → 2 → 1 cores at a 43 °C skin band and the
  voltage-sag rule while the battery was at 35 °C; the GPU stopped when every worker was shed). Now Extreme (and
  Manual) have **no regulation**: every worker joins one per second and stays; the A55s at the table's top
  (0.6.1: 1950 MHz; above the ECT's cap, back to 1794 in 0.6.2), the M3s at 1794 MHz at once (the DT's `exynos-ocp` node allows the steps above
  1794 MHz only with Samsung's over-current handler, which Starbite does not have yet), the GPU for the whole
  session, no timer. The stops left: **battery 55 °C** (its safety), the SoC at 110 °C (a clean stop just below
  S-Boot's 115 °C hardware trip, which stays armed), a lost or stuck battery/SoC reading, a core fault, the 15 %
  reserve for Light and SOS, 3.45 V and 3.5 A. No skin stop (shown, orange above 46 °C: the user's choice). Starts
  below 50 °C battery. The ~2 A in the log was not a limit: the load drew 1.8 A (ours are 3.2 / 3.5 A).
- **Manual** (Heat > Manual): a table step for the A55s (455..1950 MHz), the M3s (650..1794 MHz) and the GPU
  (off, 260..572 MHz), `SET_HEAT_MAN_*` (only listed steps; until a restart); flat out like Extreme.
- **WPA2** (the tour: associated, no handshake message 1, the AP dropped the link): the core's `bcmw_join` now turns
  the firmware's own supplicant off (`sup_wpa 0`) and power save off, as `bcmw_connect` has since W2d/W2f. The host
  model has the phone firmware's defaults (`model_ap_real_defaults`: idsup, sup_wpa 1, PM 2: message 1 withheld)
  and reproduced the failure before the fix. Starbite OS also asked for an address on association, before the keys
  (fixed: only after the handshake), and the join no longer blocks the loop: `bcmw_join_start` + `bcmw_join_poll`
  (0.6: up to 15 s frozen). The self test joins the saved `wifinet` network when it hears it (address, ping, NTP).
- **Emergency Mode's power profile** (`panel_emergency`): frames only on change (the DECON trigger masked: the
  panel shows its own RAM), the panel off with the screen, the brightness at most Mid; the settings stay as saved.
  SOS and Extreme boost to AOR `00 0C`, the vendor's UI maximum (400 nits; not HBM). Research:
  `build/em-research-panel-hbm-lowpower.md` (HBM needs gamma/ELVSS/VINT from the panel's own data: 0.6.2 with the
  user present; the panel's AOD low-power mode: 0.6.2).

### 12.13 Version 0.6: normal mode, Emergency Mode, Extreme heat (2026-11-08)

The plan is `docs/STARBITE-OS-PLAN-0.6-0.9.md`.

- **Normal mode** (the default): a wallpaper generated once at start (`kern/look.c`: Aurora, Dusk, Ocean,
  Graphite; no image files), a blurred copy made once (16 x 16 cells, a 5 x 5 box twice, a bilinear scale back),
  and "glass" cards: the blurred copy under each card, tinted, a lighter top edge. No live blur. Home: a clock and
  battery widget, the apps as glass icons in 4 columns, Emergency as a red pill.
- **The control center** (a tap on the status bar): Emergency Mode, the flashlight, Wi-Fi, the brightness, the
  wallpaper (not while heating: making one keeps the loop busy for a moment).
- **Emergency Mode**: the black big-tile screens of 0.5, and the switch-off 5 minutes after the screen goes off
  (only if the user had none; leaving Emergency Mode takes back only the one it set). The mode, whether it set
  the switch-off, and the wallpaper are in the settings record (`pad[1]` = 0x100 | mode | set << 1, `pad[2]`).
- **Extreme heat** (Heat: Off, Low, High, Extreme), within ratings only: all 8 cores at their top table steps, the
  GPU at its top step with the non-repeating workload (the confirmation names it), the panel at its top brightness
  level and the LED (once per session; its own 10-minute limit holds) for the whole session. Battery band 43 °C,
  stop 45 °C; SoC soft 78 °C; skin and current limits unchanged; needs 40 % battery.
- **The heater keeps the screen on.** After the screen-off time it shows the lock screen with touch and the
  Volume/Bixby keys locked ("Heating: touch is locked. Press Power"); a short Power press unlocks, a long one
  opens the power menu.
- **One brightness arbiter** (`panel.c`): the user's level (saved) and a boost from SOS or Extreme to the panel's
  top level while either runs; the user's level comes back when both end, and the boost is never saved.
- Host: 13 normal-mode scenes (`n-*`), `heat-pocket`/`heat-unlocked`, `MONKEY_NORMAL=1` for the monkey.

### 12.12 Version 0.5 (2026-11-08, overnight)

- The on-screen keyboard and text fields (3.1, API 24/25).
- Wi-Fi: any secured network can be joined by typing its passphrase (8..63 characters); a joined one is remembered
  in RAM ("Saved") until a restart; the store's `wifinet` network stays saved; passphrases never logged. Host:
  `sbos-wifi-test.sh` types "correct horse" on the keyboard against the model's WPA2 network (21 checks).

### 12.11 Version 0.4.2 (2026-11-08, overnight)

- Wi-Fi: **the saved network**: the store entry `wifinet` (two lines: the name, the WPA2 passphrase or nothing; the
  same entry the Wi-Fi core's T4 test reads, made on the Mac from the user's input, never in git) is read at Wi-Fi
  on, never logged; its network is listed as "Saved" and joins with the core's WPA2 handshake (`bcmw_join`, which
  blocks up to 15 s: the screen shows "Joining" first, the join runs on the next tick; the heater is never on
  then). A wrong passphrase is refused ("Could not join: the passphrase, or too far away").
- Fixed: after a blocking join the tick re-reads the clock (a stale time made the DHCP timeout fire at once).
- `starbite/host/sbos-wifi-test.sh`: open and saved joins with DHCP and the router's ping, a wrong passphrase,
  20 on / off cycles (DMA blocks reused), the heater refused while Wi-Fi is on, no names or addresses logged
  (17 checks). The model's Starbite OS mode (`M.echo_sync`) answers scans with their sync id and DHCP / ARP / ping
  on the WPA2 network too.

### 12.10 Version 0.4.1 (2026-11-08, overnight)

- UI: red hold buttons (Call 112, the alarm) are filled red-tinted cards with a light label; a control after a text
  line gets air (30 px); the SOS app's title is "SOS" like its tile.

### 12.9 Version 0.4 (2026-11-08, overnight)

- Wi-Fi (9.4): the core merged, the service, the app, open networks with DHCP and a router ping, the status bar's
  glyph; heat and Wi-Fi exclusive; the self test scans once.

### 12.8 Version 0.3.2 (2026-11-07, overnight)

- Settings saved on the phone through the `sbos-state` store entry (6.5); the screen-off A55 at 455 MHz and the
  optional switch-off (4.2); the Emergency icon (a handset with a cross, `sbos-assets.py` 207/228) and a Wi-Fi icon
  (208) for later.
- The self test adds: the screen off and on (the A55 clock both ways), the switch-off rehearsal, the slot check.

### 12.7 Version 0.3.1 (2026-11-07)

- T11 passed on the phone with 0.3 (14.5). The boot timing line (7) and the settings record (6.4, host only:
  no slot on the phone yet). Settings shows "Saved on this phone" only where a slot exists.

### 12.4 Version 0.2 (2026-11-07)

- **Heater run log**: every 10 s while heating `heater: N s bat B skin S soc C C, cores K, A55/M3 MHz, mV mA %`,
  and at the stop `heater: peak bat ... skin ... soc ... C, max mA, min mV, ran N s` (the main session's request
  after T3 Low).
- **The flashlight LED** (9.1) in Light, and optionally with SOS.
- **Info**, the emergency card (read-only card file; `kern/files.c` brings the card up on the first read).
- **The panel's power** (3.3), off until Settings turns it on; Settings: "Refresh only on change", "Panel off
  with the screen", "LED flashes with SOS" (until a restart).
- API 1 grew (numbers never reused): `SYS_SETTING_GET` 44 / `SET` 45, `SYS_FILE_READ` 91, `torch(-1)` asks the
  state. Home lays out 4 rows of tiles when there are more than 6 apps.
- Host: 350 checks; the preview 23 scenes (new: info, light-led, settings-on, heat-off); image 5210112 bytes,
  sha256 720fdbea45ac8f2f6fa0dda71d8db8da1184d56bbab7a313662c7291c52824e1, reproducible.

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

### 14.1 Results on S9 #1 (2026-11-07, image 124cf3f6, launch #53)

- **T1 / T2 passed.** The user: "works", "it's like beta 0.1 so it doesn't have much but great". pmsg:
  first frame 12963 ms after power-on (S-Boot and the loader's full POST, 7.5 s of it, included); the
  interrupt self test passed (`GIC + virtual timer, wfi sleeps`); touch up; 8 apps; **the EL0 self tests
  passed**: the spinning app was stopped by its budget, the kernel-memory read faulted (ESR 0x9200000E: a
  data abort from EL0, permission fault level 2) and nothing was read. SOS: countdown, flashing, off.
- **The heater ran once, on High** (`heater: start, level 3`): A55 to 1794 MHz answered, the M3 PMU defaults
  set, the M3s to 1469 MHz answered; then a stop (its reason was not logged in 0.1), the restores to 1066 / 1053
  MHz answered, and then **the stop check restarted the phone** ("a worker core did not turn off"): a clean
  restart, no crash, no watchdog.
- **Cause:** this firmware is PSCI 0.1. AFFINITY_INFO is not implemented and answers in w0 (`0xffffffff`
  with the upper half zero); the 0.1 code read x0 as a 64-bit number, took it for "on" and counted every worker
  core (even the never-started ones) as still running. **Fix (0.1.1):** `sb_smp_affinity` sign-extends w0; the
  decision is a pure, host-tested function (`heat_core_state`): a core whose slot says it left the heat kernel
  is off, a core PSCI still calls on while its slot is parked is logged and left alone (no load), only a core
  still in the heat kernel or one whose start timed out restarts the phone. Every stop now logs its reason
  (`heater: stopped: ...`, `heater: Off (asked)`), each state change too, and the per-core PSCI answer and
  slot state when a core is blamed. The host preview's `heat-off` scene stops a running heater with the
  phone's PSCI answer and must not restart.
- The dead-man return did not fire because there had been input (by design: it is only for boots nobody
  touched).
- **T3 step 1 (0.1.1, launch #61): Heat Low ~2 minutes, then Off: passed** ("works"). The log: start level 1,
  screen off / on three times while heating, `heater: Off (asked)`, `heater: stopped: Stopped`, `heater: every
  worker core left the load`, a clean restart from the power menu. No temperatures were logged (0.2 logs them).

### 14.2 Phone tests for 0.2 (the user present for T5..T7)

- **T5 (the LED):** Light → "Flashlight LED". The log shows `torch: gpg2 con ... dat ..., gpg3 con ... dat ...`
  before anything is switched. Expect the rear LED on; tap again: off. If the log says `torch: refused`, nothing
  was written. Undo: tap again, or any restart (the pin resets low).
- **T6 (panel power):** Settings → "Refresh only on change" on; use the apps a minute (everything must still
  update: the clock, a stopwatch, holds). Then "Panel off with the screen" on; Power short (screen off), wait 10 s,
  Power short (it must come back with the lock screen). If it stays dark: hold Power 1 s (the power menu is there
  but unseen) or force a restart (Power + Volume Down 7 s). The log shows `panel: display on did not go out`
  when a DCS write failed. Measure if possible: the current on the Battery screen with each setting.
- **T7 (heater Medium, then High, 2 minutes each):** the log now has the temperatures every 10 s and the peaks.
- **T8 (Info):** with `/starbite/emergency.txt` on the card (copy `starbite/sbos/examples/starbite/emergency.txt`):
  the card shows; without it: "Not set up". Read only.

### 14.3 Results of 0.2 (2026-11-07): the LED works; the heater and the UI failed

The user: the flashlight LED works and is good; the rest was "total slop". From `build/sbos/pmsg-0.2.bin` (the
ring had wrapped; the start of the session is lost): the heater's worker core 1 "did not start" about once a
second for minutes, every 10 s line `cores 0, 1794/1066 MHz` (the M3 cluster still powered at its restore level
from an earlier Medium / High run in the same boot), battery current flat around -400 mA, temperatures flat; hundreds
of "event queue full: heat"; the screen black, a wake "super laggy", the power menu's Boot menu did nothing (a
forced restart). Cause and fixes: 8.6 and 12.5.

### 14.4 Phone tests for 0.2.1 (the user present)

- **T9 (the heater, Low then High, CPU only):** Heat → Low 2 minutes → High 3 minutes → Off. Expect in the log
  one `heater: core N (A55|M3) running, K cores with cpu0` per core (up to 8), the 10 s lines with `cores 8`,
  the battery current far beyond -400 mA (up to about 3 A: the regulation takes a core off above 3.2 A), the
  temperatures rising, and every Settings switch,
  the power menu and the UI working while it heats (the soak's path). If a core fails, its line says why
  (`no check-in within 1 s`, `PSCI says it is still on`, `parked ...`) and the heater goes on with the rest.
- **T10 (the GPU's share, only after Engineer OS's GPU tests have passed on the phone):** Settings → "GPU in Heat
  High (test)" → hold to allow; Heat → High. Expect `heater: gpu on, 572 MHz`, `gpu 572` in the 10 s lines, and at
  Off `heater: gpu off`, `heater: gpu: vdd_g3d back`. Any `heater: gpu: ...` failure line stops only the GPU's
  share. Undo: Off; a restart resets vdd_g3d anyway.
- Then T6 (panel power) and T8 (Info) of 14.2, which 0.2 did not reach.


### 14.5 Phone tests for 0.3

- **T11 (unattended, the self test):** put `starbite-os-0.3-selftest.img` in the store as the `starbite-os`
  payload and boot it by its catalog key. It needs nobody: 5 s after the first frame it walks every screen
  (about 25 s, one step each 0.7 s), logs to pmsg, then restarts to the loader's menu; from there
  `python3 tools/store/sbfb.py reboot recovery` brings TWRP back and the pstore's pmsg has the lines
  `selftest: ...`. Expect `selftest: passed: 24 screens, apps ok 8, dead 0, too tall 0, problems 0, ...` and no
  `a loop turn took`. Never touched: the LED, the heater, the alarm, the brightness and panel settings, calls,
  messages. A touch or key stops it (the phone then runs normally and returns to the menu after 10 minutes
  without input). Undo: put the normal `starbite-os-0.3.img` back.
- **T11 result (2026-11-07, 0.3):** passed: `selftest: passed: 24 screens, apps ok 8, dead 0, too tall 0,
  problems 0, slowest draw 133 ms, slowest loop turn 133 ms, 22 s`; first frame 12508 ms (the loader's full
  POST 7514 ms of it, 7); it restarted to the menu. The slowest draws: home 133 ms, Battery 100 ms (the ring),
  the screen light 86 ms, the power menu 81 ms.
- **T12 (the user present, the look):** the normal image. Home, the tile lines, the transitions, every app's
  screen; Settings > Display > Brightness Low, then Normal (the first AOR command from Starbite OS: the screen
  goes darker, then back; a restart resets the panel anyway). Then T9 / T10 of 14.4 on 0.3 (the heater paths
  are unchanged since 0.2.1).

### 14.6 Phone tests for 0.3.2 (unattended)

- **T13 (the self test, no entry yet):** `starbite-os-0.3.2-selftest.img` as before (T11). Expect, beyond T11's lines:
  `slot: no "sbos-state" store entry`, `selftest: slot: no "sbos-state" entry: nothing written`,
  `power: A55 at 455 MHz with the screen off (was 1053 MHz)`, `selftest: screen off: A55 455 MHz`,
  `power: REHEARSAL: it would stay on: a charger is connected` (on USB), `power: REHEARSAL: the save as a dry run; would switch off now`, `selftest: screen on: A55 1053 MHz`, `selftest: passed`. Writes: none (no entry).
- **T14 (the slot, after a store backup):** pull a store backup (`tools/store/store-pull.sh`), rebuild the store with
  `--import <the backup> --volatile sbos-state=1048576`, flash it (`store-flash.sh`), then T13 again. Expect
  `fence: standing grant for 256 blk at ufs:N (volatile entry, tier 1): sbos-state`, the dry run's `fence: would
  write 1 blk ... DRY RUN`, `fence: wrote 1 blk at ufs:N (sbos-state, tier 1): FUA + cache flush`, `selftest: slot:
  dry run logged only, then saved sequence 1 to half 0, read back and picked: ok`. Then a pull of the store shows
  `SBOSSTA1` at the entry's first block and nothing else changed (compare with the backup outside the entry).
  Undo: flash the backup.

### 14.7 Phone tests for 0.4 (unattended)

- **T15 (the self test with Wi-Fi):** `starbite-os-0.4-selftest.img` as T11/T13. Beyond T13's lines, expect
  `selftest: wifi: on, the bring-up and one scan (no join), then off`, the core's step lines (`wifi: pcie: ...`,
  `wifi: chip: 7: chip 4361 revision 5`, `wifi: msgbuf: ...`), `wifi: up in N ms; scanning`, `wifi: the scan heard
  N networks (names not logged)`, `selftest: wifi: networks heard: N`, `wifi: off (the radio held in reset and
  unpowered)`, `selftest: passed: 25 screens, apps ok 9 ...` (from 0.5.9: 28 screens, with Info's card editor and the keyboard). The radio transmits only the scan's probe requests
  (as T3 did). Undo: none needed (the radio is off at the end, and a reset unpowers it anyway).
- **T16 (the user present, an open network):** the normal image; Wi-Fi → on → pick an open network (a phone's
  hotspot without a password works) → expect "Connected", the address and "The router answers in N ms".
- **T17 (0.4.2, the saved network):** with the store's `wifinet` entry (the Wi-Fi core's T4 entry), Wi-Fi → on →
  the network marked "Saved" → expect `wifi: joining the saved network (name not logged)`, `link: ... JOINED`, an
  address and the router's ping. This one can run unattended only with the user's agreement (it transmits on their
  network, as T4 did).

### 14.8 Results (2026-11-08): T15 passed on the phone (0.5.3 self test, e35cdb46)

- `selftest: passed: 25 screens, apps ok 9, dead 0, too tall 0, problems 0, slowest draw 142 ms, slowest loop turn
  359 ms, 28 s`; the watchdog armed by the loader (30 s).
- **Boot**: `first frame at 12872 ms since power-on: started at 12803, fonts and back buffer 10, battery 11, draw 48
  ms`: Starbite OS's own part is **69 ms**; the rest is S-Boot and the loader (its full POST took 7986 ms in the
  same log). The only big lever is the loader's quick POST (a loader setting).
- **Wi-Fi**: interrupts on (SPI 253), **21 interrupts** during the bring-up and scan (the INTx path works); up in
  867 ms; 2 networks heard; off after 3 s (the radio held in reset and unpowered). The slowest loop turns were two
  core bring-up steps, the power sequence (250 ms: its 50 + 200 ms waits) and the firmware download (358 ms):
  single calls into the core, splittable only with a resumable step in the core; they happen once per Wi-Fi on,
  while the screen shows "Starting".
- **Power**: the A55 at 455 MHz with the screen off and 1053 MHz back on; the switch-off rehearsal stayed on (the
  charger) and saved.
- **Slot**: no `sbos-state` entry, nothing written.
- The bare-metal wifitest T4 (open network join, DHCP lease, 3/3 pings, DNS) passed too: the same core joins for real.

### 14.9 Phone tests for 0.6

- **T18 (unattended, the self test)**: `starbite-os-0.6-selftest.img`; the phone starts in normal mode: expect
  `look: the wallpaper made in N ms` (the A55 makes it once after the first frame) and `selftest: passed: 28 screens`.
  Draw times will be higher than in 0.5 (the glass): the slowest draw is in the summary line.
- **T19 (with the user)**: the tour of normal mode (lock, home, the control center, wallpapers, Emergency Mode on and
  off) and Extreme heat (watched: it heats faster; the stops at 45 °C battery and 85 °C inside; the screen stays on).
