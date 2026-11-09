# Starbite OS 1.2-alpha5: Survival hotter and at once (2026-11-09)

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
