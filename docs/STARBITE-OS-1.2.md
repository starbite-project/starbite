# Starbite OS 1.2-alpha3: a Linux terminal (2026-11-09)

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
