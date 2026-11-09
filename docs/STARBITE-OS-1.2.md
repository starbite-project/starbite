# Starbite OS 1.2-alpha10: notifications, wake-up alarms, vibration, the Wi-Fi join fixes (2026-11-09)

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
