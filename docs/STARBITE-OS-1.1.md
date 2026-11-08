# Starbite OS 1.1-beta5: Survival heat (2026-11-08)

## beta5: the battery held too; its limit selectable

beta3's trip-off run reached 48.6 °C battery and 74 °C on the back. Survival's battery limit is now 55 / 58 / 60 °C
(default 55; 58 and 60 only after their own confirmation; 60, the usual top of a lithium cell's discharge rating,
is the highest); both Survival modes hold the battery from the limit minus 3 °C (the same shed order as the SoC hold;
back below the limit minus 4 °C) and stop at the limit itself. A Survival start is refused within 3 °C of it. Above
60 °C on the back the Heat page shows "HOT: HOLD BY THE EDGES".


## beta4: the trip-off limit is held, not a stop

beta3's trip-off run worked for the user (about 13 W, all 8 cores and the GPU, stopped at the 131 °C stop). The
user asked to remove that stop: the limit (125 to 145 °C, default 135) is now **held** like the trip-on mode's
122 °C (at or above it one load off per 250 ms, two at 2 °C over: the M3s, then the GPU, then the A55s; back on one a
second below the limit minus 2 °C), so the heat goes on to 2 % battery. A runaway still ends: a stop 5 °C past the
limit (at most 149 °C; a 150 °C reading is not valid and the lost-reading stop acts). The heat record keeps the peak
power and the energy (`peak_mw`, `mwh`); the stop's log line ends with "peak X.X W, N mWh".


## beta3: the hang of beta2's trip-off session (19:31, a watchdog reset)

beta2's stop asked for the M3 cluster's power-down (the last core's CPD hint and the APM mailbox's bit 24); the
cluster still read on, and the next session hung at its first PSCI call (no loop turn: the 5 s watchdog reset it,
S-Boot re-armed the trip). beta3: the stop gives each M3 core only its C2 hint (no cluster-off request: the cluster
stays on between sessions, as in beta1); every start puts the M3 FlexPMU words back to the boot state before its
first PSCI / ACPM call; the trip goes off only after the session's first full loop turn (the start's calls run with
the trip on and the loader's 30 s watchdog), and the short watchdog is fed right after the write; breadcrumbs to
pmsg around every CPU_ON, ACPM request and the GPU start.


1.0-rc3 (`docs/STARBITE-OS-1.0.md`) plus **Survival heat**, the user's last resort for the most heat in a survival
situation. Beta: its first phone runs are user-attended (request T23 in the shared notes). Design and limits:
`docs/STARBITE-OS.md` 12.21.

## beta2: from the user's 1.1-beta1 run (19:03) and the LED probe (18:53)

- **The big cores**: after the first session's stop "the M3 cluster still reads powered", and the next session's
  core 4 never checked in (the trip-off session ran 4 cores; the one after none: a no-show holds further launches
  while the cores share one start record). Samsung's FlexPMU bookkeeping now wraps each M3 power change (from the
  sbos-linux session's P2c): before CPU_ON CPU_INFORM = 0 and the core's GRP2 / event bits cleared (the first core:
  APM mailbox bit 24 = 0, which also clears an earlier OS's leftover cluster state); before a core leaves C2 and the
  bits set (the last: CPD and bit 24 = 1). cpu0 does it for every worker (one writer).
- **The GPU** was refused in the trip-off session by its own check ("TMU_SUB not armed"): it now accepts a trip that
  Survival switched off. The default session ran it (22 chains of 2048 fragment jobs at 598 MHz).
- **One Survival mode** (the user's spec): "Survival heat" (three confirmations) with a "Hardware trip OFF" switch
  inside; on, the start asks four times and runs **flat out, no shedding**, every core and the GPU, to the user's SoC
  stop (125 / 128 / 131 °C); off, it holds below the trip by shedding as in beta1.

### The LED chip (T23's probe, 18:53)

The probe found the bus and the chip fine (USI10's clock was running; 0x1D = 0x90, CUR1 0xCC = 1250 / 240 mA,
TIME1 0x7F, CTRL1 0x33) and rc3 refused it only at its own check: register 0x63 read **0x00**, not 0x7F. 0x63 is
reserved (no datasheet); Samsung's only use is its factory continuous-flash test (0x5F "continuous flash on", then
0x7F). 0x00 shares the bit that differs in that pair (bit 5) with 0x5F, so the chip's 125 ms flash timer, the
hardware end of a 1250 mA pulse, is not confirmed. beta2 uses the chip for the torch (pin-driven, continuous by
nature: 0x63 does not matter; 240 mA) and keeps the 1250 mA flash pulses off unless 0x63 reads 0x7F; the alarm's LED
strobes as the light instead. 0x63 is never written. Settings says so.

## Survival heat

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

Install: as 1.0-rc3, with `starbite-os-1.1-beta5.img` in the `starbite-os.img` entry and the catalog's version line
`"1.1-beta5 · heat, light, alarm, card, Wi-Fi"`. Undo: `starbite-os-1.0-rc3.img` in the same entry.
