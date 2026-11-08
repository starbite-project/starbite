# Starbite OS 1.0-rc1: release note (2026-11-08)

Starbite OS is the Galaxy S9's (SM-G960F, Exynos 9810) own small system: an emergency phone that boots in a
fraction of a second after the loader, with a light, a heater, an alarm, a medical card, Wi-Fi, a clock and the
battery, in a normal-mode phone shell with an Emergency Mode one switch away. It runs bare on libsb (no Linux),
from RAM; it writes only its own save slot. Guide: `docs/STARBITE-OS-GUIDE.md`; design and tests:
`docs/STARBITE-OS.md` (12.13 .. 12.19 for 0.6 .. 1.0); the plan after 1.0: `docs/STARBITE-OS-PLAN-0.6-0.9.md`.

## What works

- **Normal mode**: lock screen (holds against pocket touches), home with a clock widget and glass icons, wallpapers,
  the control center (tap the status bar): Emergency Mode, flashlight, Wi-Fi, brightness, wallpaper.
- **Emergency Mode**: the big black emergency screens; saves and switches the phone off 5 minutes after the
  screen goes off; at most Mid brightness and frames only on change.
- **Light**: the LED, or the screen as a light (white, warm, red).
- **Alarm (SOS)**: the screen flashes white and red at the panel's top normal brightness; the LED strobes with it.
- **Heat**: Low and High (regulated, 30 minutes, stops at 42 °C battery / 46 °C back), **Extreme** (every core at
  its top table step, the GPU, the screen, the LED: flat out until Off), **Manual** (pick each cluster's table step,
  A55 up to 1950, GPU up to 598 MHz, with warnings), **Keep the battery warm** (10 / 20 / 30 °C target). The screen
  stays on while heating with a pocket lock (Power unlocks). Live mA / W / minutes left. A record of each session
  survives a forced power-off.
- **Medical card** (Info): written on the phone, shown read only from the lock screen.
- **Wi-Fi**: scan, open and WPA2 networks (passphrase typed, kept in RAM), DHCP, the router's ping, the clock from
  the internet (NTP). The join never blocks the screen.
- **Clock** (world clock, stopwatch, timer), **Battery** (charge, estimate, a chart), **Settings** (saved on the phone).
- **Self test** (the `-selftest` image): every screen and app, Wi-Fi (and a join of the saved network), the save
  slot, the audio codec's state (read only); one line per step in the log, then back to the boot menu.

## Safety stops (heat)

| Mode | Stops |
|---|---|
| Low, High | battery 42 °C (band 40), back 46 °C (band 43), SoC 85 °C (band 75), 15 % charge, 3.45 V, 3.5 A, 30 min, a charger |
| Extreme, Manual, Keep warm | battery **55 °C** (its fire safety), the call / SOS reserve (5 / 15 / 30 %, hold to override), 3.5 A (Manual: a hold turns it off), a lost or stuck battery / SoC reading, a core fault, the SoC's hardware trip reading unarmed, a charger. Manual: an optional SoC stop (off by default) |
| always | the SoC's own hardware power cut (S-Boot's TMU trip; seen acting at 128 °C on our reading), the PMIC's and fuel gauge's protections |

Clocks: only steps in the chip's own tables (ACPM / ECT) with their own voltages; Extreme at Samsung's caps
(A55 1794, M3 1794 MHz without the OCP handler, GPU 572); Manual may go to A55 1950 / GPU 598 MHz (above Samsung's
policy caps, within the table voltages), never M3 above 1794.

## New, behind their switches (off by default; first runs with the user present)

- Settings > Alarm > **LED at rated power**: the S2MPB02 over I2C: torch 240 mA, the alarm's LED as 1250 mA camera
  flash pulses (the chip's 125 ms timer, at most one a second).
- Emergency Mode's control center > **Dim clock when off**: the panel's own low-power mode (ALPM 10 nits).
- Settings > About > **Read the panel's registers**: read only, for the brighter-screen (HBM) work.

## Known limits

- **Calls and messages**: coming soon (the modem driver is stage 0.8; emergency calls will be implemented but are
  never dialled in tests).
- **Sound and vibration**: coming soon (0.7: the codec is found; the speakers are on two MAX98512 amplifiers).
- **WPA2 on a router's 2.4 GHz side** does not join yet (the shared Wi-Fi core's 2.4 GHz fix is in test); open
  networks and 5 GHz work on the host, 2.4 GHz WPA2 fails on the phone (SET_SSID status 2).
- Heat and Wi-Fi do not run at the same time. No real RAM sleep (the screen-off state lowers the clocks). No lock
  code. The SoC reading vs the hardware trip (128 vs 115 °C) is not explained yet.

## Install (one step: the store payload)

Files (`build/store/`): `starbite-os-1.0-rc1.img` (the OS) and `starbite-os-1.0-rc1-selftest.img` (the self test),
each with a `.sha256`. The build is reproducible (`starbite/sbos/build.sh --repro` gives the same sha256).

1. The OS store (UFS) entry `starbite-os.img` = `starbite-os-1.0-rc1.img`, keeping the volatile `sbos-state`
   entry (1 MiB, the save slot; made once by `sbstore2.py build --volatile sbos-state=1048576`). For example, from a
   pulled store: `tools/store/sbstore2.py build --import store.img --volatile sbos-state=1048576 ... starbite-os.img=build/store/starbite-os-1.0-rc1.img`
   (as for 0.6.3, T14), then write the store as usual.
2. The catalog entry (catalog-live.toml), unchanged except the version line:
   ```toml
   [[entry]]
   key = "starbite-os"
   name = "Starbite OS"
   version = "1.0-rc1 · heat, light, alarm, card, Wi-Fi"
   category = "starbite"
   method = "bare"
   tag = "beta"
   files = ["ufs:starbite-os.img|sd:starbite-os.img"]
   root = "none (RAM only)"
   wdt = 30
   wdt_feed = "own"
   ```
3. For the installer (tools/installer), the same two lines go into the bundle's `store/catalog.toml` and the payload
   into the `ufs-store` image it builds (`sbstore2.py build --medium ufs --catalog store/catalog.toml ...`); nothing
   else in the manifest changes.

Undo: the previous payload (`starbite-os-0.6.5.img`) in the same entry; the save slot is kept either way.
