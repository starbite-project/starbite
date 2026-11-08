# Starbite OS: a short guide (1.0-rc3)

Starbite OS is the Galaxy S9's own emergency system: a light, a heater, an alarm, the battery, a medical card and
Wi-Fi, with big buttons and plain words. The design and the tests are in `docs/STARBITE-OS.md`.

## Normal mode and Emergency Mode

Starbite OS starts in **normal mode**: a wallpaper, glass buttons, the apps as icons on home, Emergency as a red
pill at the bottom. **Tap the top bar** for the **control center**: Emergency Mode, the flashlight, Wi-Fi, the
brightness and the wallpaper. **Emergency Mode** shows the big black emergency screens and, to save the battery,
saves and switches the phone off 5 minutes after the screen goes off; the screen is dimmer (at most Mid) and
is only refreshed when something changes.

## The lock screen

- **Hold** a round button: **Light** (the whole screen white), **SOS** (the alarm, after 3 s), **Open** (home).
  Holding protects against pocket touches.
- **Hold for the medical card** (red, once you have written one): shows your card to whoever finds the phone,
  read only. Back returns to the lock screen.
- **Power** (short): the screen off and on. **Power held**: the power menu (screen off, restart, the boot menu,
  power off).

## Home

- **Emergency** (red): **Hold for the alarm**, **Medical card**. Calls and messages: coming soon (they need the
  mobile network driver).
- **Light**: the flashlight LED, or the screen as a light (white, warm, red; low, half, full).
- **Heat**: Off, Low, High, **Extreme** (every part at its top table step, flat out until you press Off; asks
  each time with the minutes it lasts; stops only at 55 °C battery or the call / SOS reserve), **Manual** (pick the
  clocks yourself) and **Keep the battery warm** (a target of 10, 20 or 30 °C: a cold battery loses its charge).
  The **reserve** (5, 15 or 30 %, default 15) keeps enough for a call or SOS; hold "go on below the reserve" to
  override it. While heating it shows the current, the power and the minutes left. The screen stays on while it heats; after the screen-off time touch locks (a pocket):
  press **Power** to unlock.
- **Survival heat** (1.1, a last resort for a survival situation; asks three times): the most heat the phone can
  make, held just below the chip's own power cut, until the battery is at 2 % (nothing is kept for a call, the light
  or SOS). **Survival heat, trip OFF** (asks four times) also switches the chip's own heat power cut off and stops at
  the processor limit you set (125 to 150 °C); it may damage the processor. Both stop at 55 °C battery,
  and Off or Power stops them at any time. Hold the phone through a pocket or a glove.
- **Hard limits** (final, never raised): the processor limit at most **150 °C** (Survival with the trip off; at 150
  it holds about 148 °C, because the sensor's reading is not valid at 150) and the battery at most **60 °C**. 175 and
  155 °C were asked for on 2026-11-08 and declined; 150 °C was agreed.
- **Heat** (Low, High): The phone gets hot to hold: keep it in a pocket or a glove. It stops by itself at
  42 °C battery, 46 °C on the back, 15 % battery or after 30 minutes, and not while charging. A curve shows the
  battery's temperature. Heat and Wi-Fi do not run at the same time yet.
- **SOS**: the screen flashes white and red with a big SOS; the LED can strobe with it (Settings > Alarm).
- **Info**: your **medical card**. Tap "Write the card": name, blood type, allergies, medicines, conditions and
  whom to call, typed on the keyboard. (A file `/starbite/emergency.txt` on a microSD card works too.)
- **Clock**: the time, a world clock, a stopwatch, a timer.
- **Battery**: the charge, how long it lasts at this use, and a chart of the last hours.
- **Settings**: Display (brightness, screen-off time, switch off after the screen has been off), Alarm (the LED in
  the alarm; "LED at rated power", new), Heat (the graphics processor in High), About (the version, the clocks,
  temperatures; "Read the panel's registers", read only).
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
