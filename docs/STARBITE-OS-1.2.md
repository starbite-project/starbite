# Starbite OS 1.2-alpha24: Survival "Unlimited", a brownout guard, the loader names a power loss (2026-11-10)

alpha23 passed its self test and was the user's image (07:38). Its evening All out run (pulled
`build/user-1110-evening`, level 8) lasted 36 s: the cell went from 3.77 V at rest to 2.95 V at 5.56 A (1 s
minimum), SoC 151 C, then the power went (an SMPL power loss; the loader called it "FAULT (S-BOOT)"). The gauge's
resistance is about 0.13 ohm, so 5.5 A costs ~0.7 V, and the current climbs with the leakage. The user: "unlimited:
bypass everything on every system and core, max heat, no panics, except battery temp". alpha24:
- **Unlimited** (`SET_HEAT_UNLIMITED` 29, off by default, one confirmation, pad[3] bit 17): Survival starts at the
  trip-off level (4 asks) with All out, the 3.5 A and 4.5 A switches off and so B2SOVRC off, and a lost SoC reading
  (200 C or a dead TMU) no longer stops it (`heat_ctl.unlimited`). Kept: the battery limit (up to 65 C) and its hold,
  2 %, the 6 / 6.5 A for 1 s backstop, a lost battery or gauge reading, the PMIC's own buck OCP, thermal shutdown and
  UVLO (never written), the pack's PCM. Not added: clocks above alpha16's (A55 1950, M3 2 x 2314, GPU 858): the
  battery, not the clocks, ends the run (16 W browned out in 36 s), so more clock is an earlier brownout; the M3's
  OCP_WARN throttle is the buck's own protection and stays.
- **The brownout guard** (`SET_HEAT_GUARD` 30 / `SET_HEAT_GUARD_MV` 31, ON by default (the user, 19:50), pad[3] bit 18 = off, ext[1] the
  floor): a 100 ms timer (KT_HEATER 3) reads only VCELL and Current (`sb_bat_fast`); `heat_guard_sample` keeps the
  fast minimum, learns R from load steps and the OCV (V + I R); with the guard on, a sample below the floor (3.20 /
  3.35 / 3.50 V) or a 60 mV drop in one sample within 150 mV of it makes the next 250 ms step shed the smallest step
  (a clock trim, then the GPU, then a core), and nothing is added back until the cell stayed at the floor + 100 mV
  for 3 s. The guard can be switched while Survival runs. Logged every 10 s and at the stop (`heater: fast gauge`),
  shown on the Heat page and the Survival strip. The heat log's per-second peak current and lowest voltage now take
  the 100 ms samples too.
- **The boot log** names the S2MPS18's sources with the vendor's names (`boot: powered on by: SMPL (a POWER LOSS
  ...)`; OFFSRC "may be stale") and reads the charger's input current limit (CHG_CNFG_09, read only).
- **The loader** (shim/starlte, its own image): `CK_POWERLOSS` "POWER LOSS (battery brownout)" when S-Boot's
  pwronsrc has SMPL, before S-Boot's SPON / PPON "FAULT"; the crash report prints PWRONSRC / OFFSRC bit names
  (OFFSRC "(may be stale)"); and its battery check puts B2SOVRC back to 5.6 A when it finds it off (a Survival run, a
  brownout, then another OS): the one MAX77705 register the loader writes. Survival while charging is not in this
  build: it would charge a 55-65 C cell; doing it right needs the charger's buck-only mode, a separate step.

## Starbite OS 1.2-alpha23: Survival "All out", on by default; a 65 C battery limit (2026-11-10)

alpha22 passed its self test on the phone (15 apps, Wi-Fi joined) and is the user's image. The user (2026-11-10, ~07:05):
"i dont want survival heat to hold at the limit at all, add the toggle so it can go all out even though it may brownout
like it did once. its fine, but that should be the default". alpha23:
- **All out** (`SET_HEAT_ALL_OUT` 28, `heat_ctl.all_out`; the state slot's pad[3] bit 31 = off, so older records read
  as on): Heat > Survival's switch, on by default. On, Survival has no temperature hold at the user's limit (trip
  off: `heat_surv_target` / `heat_surv_estop` read as "No limit") and no current / voltage governor (alpha18's 5.0 A /
  3.35 V, alpha19's leakage steps from 165 C, trip on: the 3.4 V shed). Kept: the battery's hold (from its limit minus
  3 C) and its stop (55 / 58 / 60 C), 2 %, a reading of 200 C or more (lost: a stop after 3 s), the 4.5 A switch as it
  was (on: its 4.2 A ceiling and the 4.5 A stop; off: B2SOVRC off for the run, back at the stop and at the next boot,
  the 6 / 6.5 A for 1 s backstop), the 3.5 A switch, no LED. With the trip ON, All out still holds below 122 C: past
  the chip's own cut (~128 C) the phone switches off for certain. Off, Survival holds and governs as alpha22.
  Shown on the Heat page (the Survival button's line, the switch's line, the running line), in the confirmations, on
  the Survival screen's strip ("ALL OUT") and in the heat log (header pad 0x200, sample flag 128).
- **A 65 C battery limit** (the user, ~07:15: "the battery limit should be raised too tbh a little bit like 65"):
  Heat > Battery limit taps 55 -> 58 -> 60 -> 65 C; above 55 the kernel asks first (65 has its own warning);
  `SBOS_HEAT_BAT_HARD_C10` 650, nothing above it is accepted anywhere (the setter, the controller's start check, the
  slot's restore). The guard is unchanged relative to the limit: from the limit - 3 C (62 C) one step a second (the
  clocks, then the loads), from the limit - 1.5 C (63.5 C) every 250 ms, back up only below the limit - 4 C (61 C),
  the stop at the limit (65 C), All out or not; a start is refused within 3 C of it; the battery thermistor (the
  AP's ADC channel 4, as always) lost for 3 s stops it. The pack's PCM is never touched.
- **Isolated SoC jumps** (heater.c `soc_filter`): a reading 15 C or more away from the median of the last 3 (250 ms
  apart) is replaced by that median, once; anything closer passes as it is with no delay, so real heat and its peaks
  are never hidden (the 182 C run's saw was 6-8 C a second). libsb's per-probe median reads the same conversion three
  times, so one bad conversion would otherwise pass. A replaced high reading is logged with its probe and code,
  counted at the stop and flagged in the heat log (flag 64; heatlog.py shows S and A).
- **The 182 C the user saw was a real reading, not a glitch.** The alpha22 run (sbos-state pulled 07:15, level 8:
  trip off, No limit, battery limit 60 C, 4.5 A stop off): the SoC climbed 151 C (25 s) -> 165 (42 s) -> 170 (88 s)
  -> 176-180 (121-148 s) at 4.7-5.8 A, the governor stepping the M3 2314 <-> 2106 <-> 2002 MHz every second or two
  (a 5-8 C saw); the heat record's peak 182.0 C is 2 C over the highest 1 s sample (180 at 134 s), which a 250 ms
  reading between samples easily is. "Holding at your limit" on the screen was the current / voltage governor (it
  counts as "hot"), not a temperature limit. At 149 s the governor stepped down (3.356 V, under the 3.36 V floor
  above 165 C); from 157 s the battery's hold (57 C of 60) shed load (SoC 135 C by 164 s); the session then ended without a stop (`running` still 1) at 2.8 A / 3.67 V, the AP thermistor at 90 C: not
  the alpha18 brownout's signature; the next boot's `boot: MAX77705 ... / S2MPS18 PWRONSRC OFFSRC` lines tell why.

## Starbite OS 1.2-alpha22: Heat's "Last session" (the heat log on the phone), tappable notifications (2026-11-10)

alpha21 passed its self test on the phone (36 screens, 13 apps, Wi-Fi joined) and is the user's image; the modemsess
run passed (camped emergency-only after 30 s, bars 4). alpha22 adds:
- **Heat > Last session** (`SYS_HEAT_LOG` 62, `heater_log_summary`): the state slot's heat log (alpha6..18's
  HLG2) on the phone: the level, the length, the GPU clock, the hottest processor and battery readings, the most
  current, the lowest voltage, the most power, how it ended (or "the power went"), and four charts (processor,
  battery, current, voltage) in up to 60 time buckets (the hottest / most / lowest of each). The button is the last
  thing on the Heat page and only while heat is off: unscrolled taps stay; a page scrolled to its end shows
  everything 165 px higher (the Survival test's scrolled taps moved with it).
- **Notifications open their app:** in the control center each one is a button; a tap opens the app that posted
  it (the kernel's own: modem, heat, clock, battery).
- **A sub-page starts at its top:** a page whose title changes (Heat > Last session, Wi-Fi > Known networks, Notes >
  a note) resets the scroll; the same page keeps it.

## Starbite OS 1.2-alpha21: Modem (receive only), Files, Wi-Fi's known networks (2026-11-10)

alpha20 passed its self test on the phone (34 screens, 11 apps, Wi-Fi joined) and is the user's image. alpha21 adds:
- **Modem** (`kern/modem.c`, `apps/modem.c`): modem-core's session API (docs/MODEM.md 7.6, master merged at a8e68f4),
  receive only. The app shows the SIM, the CS / PS registration, the bars and the counters, never the PLMN, an IMEI
  or a number. Started only by the user: the app's Start opens the kernel's own confirmation ("Hold to start").
  Never at boot, never in a self-test image (refused in the kernel), never with Heat or Linux running. RADIO and EFS
  are read once (read only, about 42 MiB; refused under 64 MiB free). The CP's NV writes go to a RAM copy, restored
  from a pristine copy before every start. libsb's storage hold is on while it runs (Starbite's own saves wait and
  retry every 5 s). Polled every 2 ms, the status GETs every 10 s. Parked on Stop, on a guard (a SIM or a
  registration: a notification), before Heat or Linux starts, on every restart / power-off path, and by
  `sb_before_reset`. The modem objects are checked at build time for block-write references (none). Its ext4
  reader is built with `m_ext4_*` names so Wi-Fi keeps its own.
- **Files** (`apps/files.c`, `SYS_FILE_LIST` 92): the card's exFAT folders, read only; a text file's first 4 KiB.
- **Wi-Fi > Known networks:** the saved network and those joined since the start; Forget for the latter (RAM only);
  "Join automatically" (on by default, remembered: settings pad[3] bit 30 = off). After a scan, a known network is
  joined once per power-on, from the Wi-Fi tick; never in a self-test image (its own join step runs there).
- **APP_MAX 12 -> 20.** alpha20's pack had 13 apps for 12 places: `test-fault` (the EL0 isolation check at boot) was
  dropped silently on the phone. build.sh now fails when the pack is larger than APP_MAX.
- Home: 12 apps in 6 rows; short tiles put the icon left of the name; with Linux and Terminal, 7 rows (cap 14).
- The self test logs `selftest: modem: not started (only the user starts it); free RAM ...` and opens Modem and Files.

## Starbite OS 1.2-alpha20: Calculator and Notes; alpha19's heat changes (2026-11-09)

alpha20 = alpha19 (below: the dark Survival screen, the governor above 165 C, the self-test stall limit, none of it
run on the phone yet) plus two apps:
- **Calculator:** + - x / with a decimal point, C and Delete, chained left to right as a pocket calculator
  (12 + 7 x 2 = 38). Fixed point (6 decimals, 12 digits; 128-bit products divided by a long division, no libgcc);
  5 / 0 and overflow show an error. The keys are four rows of level pickers.
- **Notes:** up to 8 notes of 64 characters with the system keyboard, kept in the app's state (the sbos-state slot,
  so they survive a restart where the slot is set up); Edit, hold to delete.
- Two new icons (209 calculator, 210 notes) in sbos-extra.sb7: rerun `starbite/sbos/tools/sbos-assets.py` before a
  build (this branch built its own copy in build/brand/ of its worktree).
- Home has 10 tiles now, so its rows are shorter (5 rows: Light/Heat 663, SOS/Info 1250, Clock/Battery 1545,
  Settings/Wi-Fi 2015, Calculator/Notes 2485); every host test's taps moved with them. New `sbos-apps-test.sh`.
- The apps log one short line for the host tests (`calc: = 38`, `notes: saved, 2`: the count, never the text).

## Starbite OS 1.2-alpha19: a dark Survival screen by default, a faster governor above 165 C, a self-test stall limit (2026-11-09)

alpha18 on the phone: a record 173 C. From 100 to 125 s it held 161-170 C at 4.6-5.4 A, 3.34-3.43 V and 16-18 W
(the M3 stepping 2002 <-> 2106 at the 3.35 V floor), battery 47-49 C. In the last second: 173 C, 6.08 A, 3.28 V,
19.95 W, then the power went (a brownout: B2SOVRC was off). The alpha18 self test also stalled (no return in 240 s;
its pmsg overwritten by the next boot).
- **The governor above 165 C** (the leakage runaway): steps every 250 ms, from 5.6 A or 3.36 V; back up only 0.3 A
  below and 0.1 V above. Below 165 C as alpha18 (6.0 A with B2SOVRC off, 3.35 V, one step a second).
- **Survival's screen dark by default:** the readings on black, no panel boost. The user found the chips hotter
  without the white screen: under the current / voltage ceiling, the panel's ~1 W came out of the chips' budget.
  White (with the boost) is a switch in Settings > Heat, remembered.
- **The self test's own stall limit:** the regular self test still running after 180 s logs `selftest: STALLED`
  with its phase and step, and restarts into recovery, so TWRP keeps that pmsg.
- The fuel gauge's trackers, the boot: lines, B2SOVRC's handling and the 2 s heat-log saves are unchanged.

## Starbite OS 1.2-alpha18: a current / voltage governor under the charger chip's cut; the heat log survives a power loss (2026-11-09)

alpha17 on the phone (the 4.5 A stop off, on battery): a sudden power loss (SPON) at ~147 C; only the first 4 s of
the heat log survived (2.97 A at 3.87 V, then 3.78 A / 3.75 V / 14.2 W at 94 C); the MAX77705's B2SOVRC reads
0x3C: 5.6 A.
- **The MAX77705's battery-to-system cut off during Survival (the user's choice, after hearing the risk).** With the
  4.5 A stop off, B2SOVRC (CHG_CNFG_05 [3:0]) goes to DISABLE for the run. The vendor header lists only DISABLE, 4.5,
  4.8, 5.0, 5.4 and 5.6 A, and this phone is at 5.6 A, the highest threshold. The value found is noted in the slot
  first, and put back at every stop, restart and power-off, and after a crash at Starbite OS's next boot. A SoC
  reset does not reset the MAX77705, which runs from the battery; another OS booted after a crash may not restore it
  (the vendor driver's .c is not in our tree to check). The pack's own protection board stays.
- **A governor** with the 4.5 A stop off: the fine hold steps down from 6.0 A with B2SOVRC off (5.0 A if the chip did
  not take the change), every 250 ms from 0.3 A above that, or from 3.35 V on the cell; back up only 0.3 A below
  and above 3.45 V. The backstop is 6.5 A for 1 s with B2SOVRC off (6 A otherwise).
- **The heat log survives a sudden power loss:** in Survival the slot is written every 2 s (was every 30 s), and each
  second's sample keeps its peak discharge current and lowest cell voltage (log "HLG2").
- **What the last run left behind, at every boot** (read only): the fuel gauge's lowest cell voltage and highest
  discharge current since the last Survival start (MaxMinVolt / MaxMinCurr: the gauge runs from the battery, so
  they survive the power loss; cleared at each Survival start), the MAX77705's charger details and the S2MPS18's
  power-on / off sources, as `boot: ...` lines in pmsg. **A peak current near 5.6 A with the voltage still up means
  the MAX77705 cut; a lowest voltage near 3 V means a brownout.**

## Starbite OS 1.2-alpha17: Survival's 4.5 A stop gets a switch (2026-11-09)

alpha16 on the phone: it climbs fast (~130 C quickly), then stopped on "Battery current above 4.5 A". The user: the
battery has its own protection board, so the 4.5 A stop should be optional.
- **Heat > Survival > "Hold: turn the 4.5 A stop off"**: one warning screen, remembered across restarts (as the 3.5 A
  limit now is too). Off, Survival keeps drawing above 4.5 A; the soft 4.2 A ceiling is off with it.
- **A backstop stays: 6 A for 1 s** stops Survival. The loads draw ~5 A at most, so 6 A sustained means a fault. The
  battery pack's own protection and the MAX77705's battery-to-system over-current limit still act. (8 A was asked;
  6 A was picked because nothing of ours can reach it, and it catches a soft short sooner.)
- **The MAX77705's B2SOVRC** (CHG_CNFG_05 0xBC [3:0]: 0 off, 6 4.5 A, 8 4.8 A, 9 5.0 A, 0xB 5.4 A, 0xC 5.6 A) is read
  (never written) at every Survival start, logged, and kept in the heat log's header: if it is on, it is the next
  wall above 4.5 A (a cut of the system rail, so a restart).
- The battery voltage is in the heat log every second (it was already); heatlog.py prints the B2SOVRC line too.
- The battery limit (<= 60 C) and the 2 % stop are unchanged.

## Starbite OS 1.2-alpha16: Survival with the GPU overclock, a fine hold, the 150 C reading fixed (2026-11-09)

From the user's alpha7 Survival run on battery (19:30, the heat log in the slot):
- **The "150 -> 130 -> 135" saw was a reading bug, not the chip.** At a steady 11.7 W the hottest reading dropped from
  150 to 129 C in one second (t = 223 -> 224 s, and again at 375 -> 376). alpha4 allowed SoC readings up to 199 C in
  the TMU's sample check, but the sample filter around it still dropped every sample above 150. The hottest probe
  vanished and the next one (~130 C) was shown. Fixed in libsb tmu.c (the filter's range is -30 .. 199 C). With "No
  limit" the reading now keeps rising past 150 (the chip's real hottest spot); at 200 C or above it counts as lost
  and stops Survival.
- **The draw was ~3.1 A (11.7 W)** with 3 A55 at 1950, 2 M3 at 2314 and the GPU at 598 MHz, and the GPU at ~731 mV:
  ACPM had put its own voltage back (fixed in alpha12).
- **A fine hold.** At the limit Survival now steps the M3 and GPU clocks down together, one ECT level a second (M3
  2314 -> 2106 -> 2002 -> 1924 -> 1794 -> 1690 -> 1586 -> 1469 MHz; GPU 858 -> 806 -> 754 -> 702 -> 650 -> 598), before
  it sheds a whole load. Back below: every load first, then the clocks up. Each step is a few percent of the power,
  not a whole core. The M3's 1924 / 2002 / 2106 steps are allowed for 2 busy cores at most.
- **The GPU at 858 MHz** in Survival (H2b / H2c on the phone: 858 held 30 s, the GPU's cycle counter read 857), at
  950 mV held (alpha12), PLL_G3D with the vendor's sequence (S 0). A failed lock goes back to 598 for the session.
- **"No limit" asks once** (was 8 holds): Heat > Survival > Hardware trip OFF > "Hold: remove the SoC limit" and one
  warning screen. Survival's SoC limit (or none), its battery limit (55 / 58 / 60) and the trip-off switch are
  remembered across restarts (the state slot).
- **A soft current ceiling** 0.3 A below the 4.5 A stop: from 4.2 A the hold steps down, back up only below 3.9 A,
  so a single gauge spike above 4.5 A (which ends Survival) is less likely. The 4.5 A stop and the 3.5 A switch are
  unchanged.
- **The GPU's compute job** (H2d on the phone: 40 jobs a second at 858 MHz, each 4.3 G FMAs, the GPU zone at 94 C
  alone, the results bit-exact with the CPU's FMA loop) replaces the memory-bound clear jobs in Survival: an FMA-loop
  shader compiled by Mesa's Bifrost compiler (tools/mesa-alujob.c -> kern/gpualu.h), on job slot 1. If it ever fails
  (a fault, nothing written), the clear jobs take over for the rest of the session.

## Starbite OS 1.2-alpha10: notifications, wake-up alarms, vibration, the Wi-Fi join fixes (2026-11-09)

- **Notifications** (alpha10): a dot next to the time while any is unread; tap the status bar for the control
  center, which lists the newest four (with the time) and has Clear. Posted by apps (notify()) and by the phone
  itself: a missed alarm, heat that stopped by itself, the battery at 15 % and 5 %, the Clock timer. Kept until a
  restart.

The road to "a proper OS" (the ranked gap list: calls and SMS with modem-core; then Wi-Fi, vibration, alarms,
standby battery, notifications, sound, a lock code, app install, speed). Items 2, 3, 4 and 6 started here.

- **Alarms** (Clock > Alarms): four alarms, each on / off, its time (+/- 1 h, 10 min, 1 min) and Once / Every
  day / Weekdays. At its minute the screen comes on, Clock shows WAKE UP with Stop and Snooze (9 minutes), and the
  phone buzzes every 2.5 s, at most 10 minutes. Power snoozes; Back snoozes too (never a silent stop). The phone
  does not switch itself off while an alarm is set (it could not ring switched off). Saved with Clock's state.
- **Vibration** (alpha8): the S9's motor (an LRA) through PWM timer 1 (gpp3-1), the motor boost (gpg1-7) and the
  MAX77705's haptic bits (MAINCTRL1 bias, MCONFIG 0xC2), the vendor DT's alert setting (48.8 us, 70 %). The first
  use measures the PWM timer's clock and refuses if it does not tick. The Clock timer ends with three buzzes. Apps:
  vibrate(), SET_VIBRATE. The test image `starbite-os-vibtest.img` buzzes three times, stops a pattern midway and
  restarts into recovery.
- **Wi-Fi join** (alpha7): wifi-core's fixes in Starbite's non-blocking join: 5 GHz preferred (every self test so
  far took 2.4 GHz and timed out), every firmware event logged while joining, 300 ms of settling after the directed
  scan, 256 rx buffers. The Linux guest's bounce load now stops below the radio's DMA window.
  **On the phone (alpha7 self test, cab82e5a, 2026-11-09): PASSED, the first WPA2 join**: the 5 GHz side (chanspec
  0xe32a, -56 dBm), the four-way handshake answered, keyed 2292 ms after SET_SSID (4944 ms in all), DHCP bound
  (lease 7200 s), the gateway pinged in 21 ms, DNS answered, the clock corrected over the network (+6 s); 26 screens,
  9 apps, 0 problems.

## 1.2-alpha6 (2026-11-09)

- Survival: 2 M3 cores at 2314 MHz (the phone's own beta6 / beta7 measurements: 9.5 / 9.7 W against 9.1 / 9.4 W for
  4 x 1794; alpha5's 4 x 1794 drew under 3 A), the A55s and the GPU, all at once.
- The heat log: one sample a second for 10 minutes, then one every 10 s (SoC, battery, current, voltage, the M3
  clock, cores, flags, the PMIC's OCP_WARN1, and the M3 throttle counter after Settings > About > OCP probe), saved
  in the slot and kept until the next session; `starbite/tools/heatlog.py <store image>` prints it.

## 1.2-alpha5: Survival hotter and at once (2026-11-09)

The user's findings on alpha4 (the main session's relay): the measurement phase rose to ~130 C, fell to ~100 C and
climbed back slowly; Survival rarely drew more than ~3.5 A even with the limit off; no flashlight in Survival; the
screen as heat too.

- **No M3 measurement.** 1.1-beta6 measured 4 x 1794, 3 x 1794, 2 x 2314 and 1 x 2704 MHz for 4 s each, then picked
  the most watts (on the phone 4 x 1794). The one- and two-core steps run near 1.2 V and heat one spot under the
  hottest probe (the ~130 C); the pick spreads the same heat over four cores (the ~100 C), and the climb after that
  is the whole phone warming. Now every load starts at once: 4 M3 cores at 1794, the A55s at 1950, the GPU at 598
  (heater.c starts one core per 250 ms tick: all 7 in about 2 s). Cold (SoC below 20 C or battery below 5 C): the
  A55s first for 30 s, as before.
- **The screen as heat.** Survival shows the screen full white at the panel's top level (AOR 00 0C, the level
  Extreme already used) with a dark strip on top: the processor and battery temperatures, the battery's current and
  charge, and the reason line. A tap or key brings the screen it covered back; the Heat page's "White screen: more
  heat" shows it again; it ends with Survival. While it shows, the power key keeps the screen on (as the screen light).
- **No LED in Survival** (Extreme keeps it).
- **The heat probe image H1** (`starbite-os-heatprobe.img`, unattended, own 30 s watchdog, restart into recovery):
  ACPM's fvmap (A55, M3, G3D, MIF levels with ACPM's voltages), the S2MPS18's registers, the A55 at 1794 and 1950 MHz
  with the registers that changed (finds the A55 buck), the screen's battery current (home, white, white at the top
  level), and the panel registers HBM would need (CA, B3, B5, F4, B1, B8, ACL 55 / B4). All reads, plus A55 levels
  Survival already sets and the screen light's white.
- **HBM: possible with Samsung's own values, not sent yet.** Samsung's sunlight level on this panel (the vendor
  s6e3ha8 star_a3_s0 tables) is: gamma from the panel's own HBM gamma (B3), AOR 00 0C, ELVSS and VINT from the
  vendor's table rows for the HBM luminances, ACL on at 8 % (the vendor's HBM rule), IRC from its table. H1 reads the
  registers; the packets are computed from that dump and reviewed before an image sends them, and the panel's
  pre-HBM register values are written back at the end.
- Next (separate test images, one at a time): H2, the GPU above 598 MHz (PLL_G3D set directly, vdd_g3d 975 ..
  1100 mV, 60 s per step); H3, the A55 at 2002 and the four M3 cores at 1924 / 2002 / 2106 MHz.
- Host: unit test 518 checks; the Survival scenario test checks no measurement, every core running, the white
  screen and its strip, a tap back, no LED, everything put back.

## 1.2-alpha4 (2026-11-09, the main session)

- Survival (trip off) "No limit" behind 8 holds: no SoC hold or stop of ours by temperature; the battery limit and
  2 % still stop. SoC readings are trusted up to 199 C (a reading at 200 or above counts as lost: a stop).
- The 3.5 A battery-current limit switch applies to Survival too.

## 1.2-alpha3: a Linux terminal (2026-11-09)

- **Terminal** (home, next to Linux; behind the same Settings switch): a Linux shell on the big cores, large
  (45 x 33 characters) above Starbite's keyboard. Enter runs a line; Volume steps through the history; Bixby is
  Tab; hold Bixby for home. Needs the store entries `guest-term.img`, `guest-term.dtb` and `guest-term-rd.img`.
- The Linux view copies only the rows that changed (faster, less power), when the guest says which.
- One Linux at a time: opening the other one closes the first.

## 1.2-alpha2 (2026-11-09)

alpha2: the self test's tap check reads a frame the view really drew (the P3a run on the phone timed out waiting
for a moment without an undrawn frame); nothing else changed.

## 1.2-alpha1 (2026-11-08)

1.1-beta7b plus the sbos-linux session's guest (Linux 7.1 on the four M3 cores), wired in as docs/STARBITE-OS.md
12.22 describes.

- **Off by default**: Settings > About > "Linux apps (test)" asks first and lasts until a restart. Then home has a
  **Linux** tile: Linux starts on the big cores and shows full screen at 2x; touch, Volume and Bixby go to it. Hold
  Bixby (or tap the bottom bar) for home; Linux stops by itself 60 s after it was left or the screen went off.
- **Never with Heat**: Heat (Survival included) is refused while Linux runs, and Linux while Heat runs.
- **Store files**: `guest-linux.img`, `guest-linux.dtb`, `guest-linux-rd.img` (starbite/guest/probe/build.sh
  bundle). Without them the switch says so and nothing else changes.
- **The self-test image** `starbite-os-guesttest.img` (P3a): unattended, its own 30 s watchdog, two Linux cycles (one
  shown with a tap that must reach the app, one in the background), each must end with the M3 cluster off; then a
  restart into recovery for pmsg.
- Survival heat, the LED and everything else: as 1.1-beta7b.
