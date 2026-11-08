# Starbite OS 1.1-beta1: Survival heat (2026-11-08)

1.0-rc3 (`docs/STARBITE-OS-1.0.md`) plus **Survival heat**, the user's last resort for the most heat in a survival
situation. Beta: its first phone runs are user-attended (request T23 in the shared notes). Design and limits:
`docs/STARBITE-OS.md` 12.21.

- **Survival heat** (Heat > "Survival heat", three confirmations): every core and the GPU at the chip's table tops,
  the panel at its top, the LED; held just below the chip's own power cut (the load shed at our 122 °C reading; the
  chip cut at our 128 °C in two runs) until 2 % battery. Stops that stay: 55 °C battery, the chip's trip, a lost fuel
  gauge, a current above 4.5 A after shedding, the PMIC's own protections, Off / Power.
- **Survival heat, trip OFF** (four confirmations; the user's explicit request): the chip's hardware heat trip
  switched off (read back; refused if the register does not take it), a 5 s watchdog fed only by the heat loop, the
  user's SoC stop (125 / 128 / 131 °C, default 131) with the load held 3 °C under it, and the trip restored exactly
  at every end (stop, Off, switch-off, restart; any reset re-arms it through S-Boot).
- Not in 1.1: the M3 above 1794 MHz (past the cluster buck's current rating without Samsung's over-current handler),
  camera sensors, HBM, Wi-Fi TX, a shader workload (`docs/STARBITE-OS.md` 12.21 says why).

Install: as 1.0-rc3, with `starbite-os-1.1-beta1.img` in the `starbite-os.img` entry and the catalog's version line
`"1.1-beta1 · heat, light, alarm, card, Wi-Fi"`. Undo: `starbite-os-1.0-rc3.img` in the same entry.
