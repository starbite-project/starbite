# Starbite OS: a short guide (0.5.14)

Starbite OS is the Galaxy S9's own emergency system: a light, a heater, an alarm, the battery, a medical card and
Wi-Fi, with big buttons and plain words. The design and the tests are in `docs/STARBITE-OS.md`.

## The lock screen

- **Hold** a round button: **Light** (the whole screen white), **SOS** (the alarm, after 3 s), **Open** (home).
  Holding protects against pocket touches.
- **Hold for the medical card** (red, once you have written one): shows your card to whoever finds the phone,
  read only. Back returns to the lock screen.
- **Power** (short): the screen off and on. **Power held**: the power menu (screen off, restart, the boot menu,
  power off).

## Home

- **Emergency** (red, at the top): **Call 112** shows what would happen (there is no modem driver yet: it never
  dials), **Message** (three ready texts, not sent yet), **Hold for the alarm**, **Medical card**.
- **Light**: the flashlight LED, or the screen as a light (white, warm, red; low, half, full).
- **Heat**: Off, Low, High. The phone gets hot to hold: keep it in a pocket or a glove. It stops by itself at
  42 °C battery, 46 °C on the back, 15 % battery or after 30 minutes, and not while charging. A curve shows the
  battery's temperature. Heat and Wi-Fi do not run at the same time yet.
- **SOS**: the screen flashes white and red with a big SOS; the LED can strobe with it (Settings > Alarm).
- **Info**: your **medical card**. Tap "Write the card": name, blood type, allergies, medicines, conditions and
  whom to call, typed on the keyboard. (A file `/starbite/emergency.txt` on a microSD card works too.)
- **Clock**: the time, a world clock, a stopwatch, a timer.
- **Battery**: the charge, how long it lasts at this use, and a chart of the last hours.
- **Settings**: Display (brightness, screen-off time, switch off after the screen has been off), Alarm (the LED in
  the alarm), Heat (the graphics processor in High, a test), About.
- **Wi-Fi**: turn it on, pick a network. Open networks join at once; a secured one asks for its passphrase (kept in
  memory until a restart, never written to the log). The network saved on the phone is marked "Saved". Once
  joined it gets an address, checks the router and sets the clock from the internet.

## Keys

- **Volume** moves the focus (a frame), **Bixby** selects (a hold button needs Bixby twice), **Bixby held** is
  Back.
- The keyboard is touch only.

## Saving

- Settings, the medical card and the apps' own data are saved on the phone once the phone has its save slot (the
  `sbos-state` store entry, an install step). Until then they last until a restart; Settings and Info say which.

## What is not there yet

Calls and messages (no modem driver), sound and vibration (no drivers), real sleep with the memory kept (the
screen-off state lowers the clocks instead), a lock code.
