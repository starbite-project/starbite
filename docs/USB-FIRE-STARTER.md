# USB-C fire starter (classic Starbite OS 1.2-alpha30: Emergency > Fire starter)

The phone puts 5 V on its USB-C port in short, deliberate pulses. A cut USB cable feeds a pencil lead, and the lead
glows and lights tinder wrapped around it. Background: `docs/USB-FIRE-RESEARCH.md` (the chip's OTG behaviour) and
`docs/USB-FIRE-PLAN.md` (what the 2026-11-12 tests fire-1 / fire-2 showed).

## 1. The ceiling: what the MAX77705 can deliver

- The only path from the battery to the USB-C port is the charger's **OTG boost** (CHG_CNFG_00 MODE 0x0A). Its
  output switch (BYP -> CHGIN) has a **hardware current limit**, OTG_ILIM in CHG_CNFG_02 bits 7:6. It has four
  steps: 500, 900, 1200 and **1500 mA**. No register sets anything higher.
- At the 1500 mA step the chip trips somewhere between **1.5 and 1.73 A** (1.61 A typical, sibling datasheets). It
  holds that current for about 30 ms, opens the switch, and retries after about 300 ms. Into a load that wants more,
  that gives 30 ms of power out of every 330 ms (about 9 %).
- **Stock Android** sets 900 mA (`CONFIG_CHARGER_MAX77705_OTG_LIMIT`) and switches OTG off on the first overload.
  Starbite already runs **1500 mA**, the chip's top step. That is the "above the official limit" result: about 1.7x
  stock, with the chip's own trip point (~1.6 A) as the real ceiling.
- Going past the trip point would mean defeating the chip's own protection: a faster re-enable than its 300 ms rest,
  or overriding a fault. Both are forbidden here. CHGPROT (CNFG_06) is a write guard for CNFG_01..03, not a
  protection. It is unlocked only to set OTG_ILIM, and put back at the end.
- So the most power the port gives is about **5.1 V x ~1.6 A ~ 8 W**. That needs a load of about 3.4 ohm or more:
  any lower and the chip sits in its 9 % hiccup.

## 2. The feature

Emergency > **Fire starter** (`starbite/sbos/apps/emergency.c` P_FIRE; kernel `kern/survival.c` `surv_fs_*`).

- **Pulse:** 0.5 s, 1 s or 2 s. **Pulses per hold:** Single, or Repeat (up to 6, with a rest of max(1 s, the
  pulse) between them). **Current limit:** 0.9, 1.2 or 1.5 A (the chip's own steps; 1.5 A is the default).
- **Arm** switch, then **HOLD TO FIRE**: hold the red bar 1 s to start. It fires **only while the finger stays on
  the screen**. The kernel's UI (`kui_finger_down`, `ui.c`) cuts the output at the lift itself, or if the finger
  slides more than 250 px away. The keys cannot fire. Leaving the page, Back, the screen going off or switching
  Arm off also stop it.
- **Live readout:** FIRING n/m with the time left, then the last pulse's mean and peak mA at the port, watts, the
  port voltage band, the battery's low point, the battery temperature, and why the last hold ended.

### Safety (all in the kernel, checked every 20 ms while the output is on)

| Condition | Action |
|---|---|
| Finger lifted / slid off / screen off / Stop | output off at once |
| BYP_DTLS bit 0: the chip at its current limit (OTGILIM) | off; "move the wires apart" |
| BYP_DTLS bits 1-3 (the boost's limit, unknown flags) | off (fault) |
| CHG_DTLS 0x0A (the charger's thermal shutdown), BAT_DTLS 0x6 (battery overcurrent) | off (fault) |
| MODE no longer OTG (the chip switched itself off) | off (fault) |
| VBUS below 3.5 V (VBADC code 0) after the first 100 ms of a pulse | off (collapse) |
| battery below 3.3 V, or 3 % | off |
| battery >= **45.0 C** (read 5x a second; no start at >= 43 C), USB connector >= 60 C, charger >= 65 C | off (hot) |
| a pulse at most 2 s, at most 6 pulses a hold | then a mandatory cooldown: 5 s + 3x the time on (max 60 s), 30 s after any fault |

The battery pack's protection, the fuel gauge, B2SOVRC and the PMIC's thermal shutdown are never written. The only
writes are CNFG_00 (mode), CNFG_02 (OTG_ILIM) and CNFG_06 (CHGPROT, the write guard). All three are put back after
every hold and before any restart. Self-test images refuse to fire.

### Log (pmsg)

- `survival: fs: ON (...)`: the start, with the limit, the mode and the temperatures.
- `survival: fs: p<n> +<ms> ms: VBUS code ..., battery mV / mA, about N mA at the port; bat / conn / chg dC`: ten
  a second while on (the ladder's trace).
- `survival: fs: pulse <n> (...)`: each pulse's mean / peak mA, W, the lowest VBUS code, the battery's low point.
- `survival: fs: the chip's status: BYP_DTLS ...`: every change of the chip's flags.
- `survival: usb out: off after ...: <why>` and `the charger is back as it was`.

## 3. Measurements on S9 #2

**Run 1 (2026-11-16 ~15:40, build 1.2-alpha30c, 0.5 mm lead, 6 cm gap, override 2 at 1 s; pmsg
`build/survival/fs-run1-pmsg.txt`): no glow, no spark.**
- Port current pinned at the chip's limit: about 1.42-1.49 A out of the port (from the cell's extra draw), BYP_DTLS
  bit 0 toggling every ~40 ms (84-101 limit events in a 1 s pulse), 17 re-arms.
- VBUS band 0 (below 3.5 V) for the whole pulse: the lead + wraps were under ~2.4 ohm, so the limiter, not the lead,
  set the current.
- The cell gave ~10 W (3.5 V, ~2.9 A). The power into the lead was at most ~5 W (1.45 A x < 3.5 V); the rest burned in
  the chip's OTG switch (VBYP - VBUS) x I. The screen's "7.4 W" assumed 5.1 V at the port: wrong, fixed in 30d (the
  gauge's VBYP, FG 0xB3).
- Battery 21 C, USB connector 21 C, charger up to 37 C: nothing near a stop.
- Lesson: a 0.5 mm lead of this brand is ~0.3-0.4 ohm/cm. Under a pinned current the heat per cm depends only on the
  lead's ohm/cm, so the gap length does not help; a thinned spot (half the thickness over 5 mm: 2-4x the local
  ohm/cm, less mass) does. A long enough lead (>= 3.5 ohm, ~9-10 cm) runs within spec with no limiting.

**Run 2 (build 30d, notched 0.5 mm lead):** the ATORCH USB tester could not show anything (it needs a few seconds to
start and could not hold the load); the user smelled the lead's binder but saw no smoke; the lead was "a little warm".

**Run 3, MAX (build 30f, notched lead; pmsg `build/survival/fs-max1-pmsg.txt`):**
- **VBYPSET is ignored in OTG mode:** CNFG_11 codes 0x08..0x54 all read back, and the gauge's VBYP stayed 5027 mV
  every second. The boost's output in OTG is fixed at ~5.0 V. Override 2 and MAX's voltage walk are removed (30g).
- **The best pulse so far:** the 2nd second, port band 3 (~5 V and up), mean 954 mA, peak 1529 mA, only 3 limit events:
  a load of about 5 ohm taking **~5.7 W** (5.6 W from the cell, nothing lost in the chip's switch). Then the load
  went open (14-74 mA for the remaining 10 s): the lead broke at its notch, or a wrap came loose.
- No temperature anywhere near a stop (battery 28 C, connector 27 C, charger 31 C).
- The "crash" after the run was the forced key restart (rst 0x00010000) used to reach fastboot, not Starbite OS.

### The measured ceiling

| | value | source |
|---|---|---|
| port voltage while under the limit | ~5.0 V (VBYP 5027 mV), not adjustable in OTG | fs-max1 |
| current limit | 1.5 A step, trips at ~1.53 A (peak 1529 mA seen) | fs-max1, run 1 |
| most power into a load | **~7 W** in theory (5.0 V x 1.5 A at ~3.4 ohm); **5.7 W measured** at ~5 ohm | fs-max1 |
| a load under ~3.3 ohm | pinned at ~1.45 A, the port below 3.5 V, the cell's ~10 W mostly burned in the chip's switch | run 1 |

Best loads for that ~7 W: **fine steel wool (0000)**, a few strands stretched between the wires (lights at once);
an intact thin lead (0.3 mm, or 0.5 mm H/2H) firmly clamped, about 3.5-5 ohm, with the tinder pressed onto its middle;
a notch only lightly (it burns through in a second, as run 3 showed).


## 4. Controls without touch, and the touch fix

- **Keys (1.2-alpha30c):** on the Fire starter page Volume Up / Down move, **Power short** selects (Arm, the
  choices), **Power held 1 s** fires and keeps firing only while Power stays down (input.c's fire key mode, set by the
  page every 200 ms through SET_SURV_FS_KEYS, expiring 2 s after the page stops refreshing it). On that page Power
  neither switches the screen off nor opens the power menu; Bixby held is Back.
- **Touch after a boot without the menu:** the loader's menu powers the S6SY761 (LDO35 0xEC, LDO43 0xF0, the four
  CMU_PERIC0 gates). A fastboot RAM boot skipped it ("sbos: no touch"). main.c's touch_bringup() now does the same,
  with the menu's checks, when sb_touch_start() fails.

## 5. S9 #2's override (the user's test phone; attended)

The user asked to go above the chip's limit on S9 #2. No register does that: the OTG switch's limit is an analog
clamp (OTG_ILIM top step 1500 mA, ~1.6 A typical), and there is no OCP or retry disable bit (Samsung's and mainline
headers). What the override does, for pulses of at most 1 s:
- **1 Fast re-arm:** on the chip's limit flag or its own switch-off, OTG off and straight back on (the chip waits
  ~300 ms). More duty, same amps; costs heat in the chip's OTG switch.
- **2 + boost V:** VBYPSET (CNFG_11 0xC2, 0 = 5.0 V; the step is undocumented) raised to a code chosen on screen. More
  volts at the same amps, only useful with a load of ~3.5-4 ohm; with a low-ohm load it only adds chip heat.
- Still stops at 45 C, 3.3 V, the chip's thermal shutdown and battery-overcurrent flags, and after 50 re-arms in a
  pulse. CNFG_00 / 02 / 06 / 11 go back after every hold and before a restart; surv_init() puts VBYPSET back to 0 at
  the next start if a crash left it raised.
- **Steel wool preset (30e):** 3 s at 1.5 A, no override: the chip's own limit-and-retry (30 ms on, 300 ms off) is
  let run as designed, for a few strands of fine steel wool (a near-short that lights from sparks).

## 6. Heater wire (30h): the reliable path

Override **MAX / heater wire**: steady output while held, no length, no cooldown, LOAD OPEN under 50 mA. The **Load**
line shows the resistance live: VBYP / I over each 1 s window without a limit event (the chip's switch drops little
below its limit), or "under ~3.4 ohm, the chip limits it". Trim a nichrome / kanthal wire (an old hair dryer's or
toaster's element, a vape coil) to **3.4-4.5 ohm**: that is where the port gives its full ~7 W (5.0 V x 1.5 A at
3.4 ohm, 5.6 W at 4.5 ohm), and a thin coil glows. A USB arc lighter also runs on MAX.

## 7. How to use it (0.5 mm pencil lead)

1. Unplug any charger or computer. Work over stone, soil or a metal tray, with water ready.
2. Cut USB cable: red = +5 V, black = ground. Wrap each bare wire 3-4 tight turns around the lead, **6 cm apart**.
3. Emergency > Fire starter. Pulse 2 s, Repeat, 1.5 A. Arm. Hold the red bar.
4. If it stops with "drew more than the limit", the wires are too close: move them 5 mm apart. If the mA stays well
   under ~1000 and nothing glows, move them 5 mm closer. Aim for the highest mA that does not stop it.
5. Toilet paper loosely round the middle of the lead, then fire again. Blow gently once it smoulders.
