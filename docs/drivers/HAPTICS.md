# Starbite OS: haptic patterns

Drivers track, 2026-11-12. Written and host-tested. **The patterns and any duty other than 70 % have not run on
the phone yet.** vib.c's fixed 70 % pulses have (1.2-alpha11 and later).

Files: `starbite/sbos/kern/haptic.c`, `haptic.h`; three small hooks in `vib.c` (and `kern.h`). Host test:
`kern/host/haptic-test.sh` (the Mac; haptic.c on the real vib.c built with `-DSB_HOST`).

## 1. Wiring (unchanged, vib.c)

The motor is an LRA. Samsung's GPL DT `exynos9810-starlte_motor.dtsi` and `_if-pmic-max77705.dtsi`:

- PWM timer 1 of 0x10510000 on gpp3-1 (function 2, pull-down);
- boost enable gpg1-7, output high;
- MAX77705 at I2C 0x66 (HSI2C 0x14360000): MAINCTRL1 0x02 bit 7 (bias), MCONFIG 0x10 = 0xC2 (LRA, enable, PWM / 128).

DT haptic node: `max_timeout = 10000` ms, 7 frequencies, the first (used) period 48828 ns / duty 34179 ns
(20.48 kHz in, 160 Hz at the motor), `normal_ratio = 70`, `overdrive_ratio = 95`.

## 2. Strength

Samsung's `sec_haptic` maps an intensity onto the PWM duty between half the period (no drive: the MAX77705's PWM
input is centred) and the maximum duty. The 50 % centre is from memory of `sec_haptic.c` (not in our sparse tree);
`sec_haptic.h` has `MAX_INTENSITY 10000`, the ratios come from the DT.

Here: duty = 50 % + 20 % x strength x gain, with

| Intensity | Gain | Full-strength duty |
|---|---|---|
| 0 off | 0 | nothing is driven |
| 1 light | 50 % | 60 % |
| 2 medium | 75 % | 65 % |
| 3 strong (default) | 100 % | 70 % = the DT's rated drive, vib.c's fixed value |

A non-zero strength gets at least 54 % (an LRA hardly moves below that). The duty never exceeds 70 %:
`vib_set_duty` clamps to 500..700 per mille. The overdrive ratio (95 %) is never used.

## 3. Safety limit

- vib.c already ends any single pulse after 5 s. The DT's own cap per request is 10 s.
- haptic.c adds: an "on" segment is at most 2 s; the motor is never driven for more than 5 s in total without a
  rest of at least 250 ms (otherwise a forced 1 s rest is inserted and logged once); a looped pattern ends by itself
  after 60 s (the caller plays it again if the phone still rings).
- Why 5 s: it is vib.c's existing limit, half of Samsung's, and a ring pattern never needs more than 0.8 s at a time.
  The standard ring drives the motor 40 % of the time, the alarm 50 %.

## 4. Patterns

| Pattern | Segments (ms on @ strength / ms off) | Loop |
|---|---|---|
| ring | 800 @100, 400 off, 800 @100, 2000 off | yes |
| notification | 60 @100, 120 off, 60 @100 | |
| alarm | (500 @100, 300 off) x 3, 1000 off | yes |
| key tick | 20 @60 | |
| success | 30 @70, 80 off, 50 @100 | |
| warning | 50 @100, 150 off, 50 @70 | |
| error | (40 @100, 70 off) x 2, 40 @100 | |

A looped pattern replaces anything. A one-shot replaces a one-shot but not a looped pattern (it returns -16).
`vib_off()` from anyone (a call answered) stops the pattern. vib.c's own `vib_pulse` / `vib_buzz` keep working.

## 5. API (`haptic.h`)

`haptic_play(HAPTIC_RING ...)`, `haptic_play_segs(segs, n, loop)`, `haptic_stop()`, `haptic_playing()`,
`haptic_set_intensity(0..3)` / `haptic_intensity()`, `haptic_why()` (plain words when the motor is refused),
`haptic_duty_for(strength)`. The intensity lives in RAM; the settings session can save it.

## 6. What changed in vib.c

Additive, about 50 lines:

- `duty_pm` (default 700) and `vib_set_duty(pm)`: the duty for the next pulse, and for a running one at its next
  PWM period (TCMPB1, auto-reload). With 700 the duty is computed exactly as before.
- `vib_wait(ms)`: the motor off and vib.c's KT_UI 9 timer armed for a gap.
- `vib_set_end_hook(fn)`: called with 1 when a pulse or gap ends (the last branch of `vib_timer`) and with 0 from
  `vib_off`.
- `kern.h`: the three prototypes.

## 7. The probe

`int drv_haptic_probe(int level, unsigned *wait_ms);` `void drv_haptic_abort(void);`

| Level | Budget | Touches | Pass line |
|---|---|---|---|
| 0 | 1000 ms | reads: TCFG0, TCFG1, TCON, TCNTB1, TCMPB1, gpp3 CON, gpg1 CON / DAT; MAINCTRL1 and MCONFIG only if the MAX77705 bus is already up | `drvprobe: haptic: PASS read only: ...` |
| 1 | 100 ms | nothing | `drvprobe: haptic: PASS skipped (attended only: a silent haptic test is impossible)` |
| 2 | 12000 ms | the notification taps, a pause, 3 s of the ring pattern, then 300 ms pulses at intensity 1, 2, 3 | `drvprobe: haptic: PASS notification, 3 s of ring, then three strengths played; ...` |

Fail lines: `FAIL a register did not read`, `FAIL <haptic_why()>` (the motor refused), `FAIL a segment did not end
on time (the KT_UI 9 timer did not fire): the motor stopped`, `FAIL the motor still ran after ...`, `FAIL did not
finish within its budget`. Level 2 needs KT_UI 9 dispatched to `vib_timer` (ui_ktimer does it in every image); the
probe stops the motor itself if a segment overruns by 300 ms. The abort stops the motor and restores the intensity.

## 8. Risks and what is left

- The 50 % centre is from memory. If the MAX77705 is not centred, lower intensities are simply weaker or nothing;
  the rated 70 % stays the ceiling, so nothing is driven harder than today.
- Not wired: the modem's ring (`haptic_play(HAPTIC_RING)` / `haptic_stop()`), notifications, the keyboard tick and
  the Settings intensity. Those belong to the modem and UI sessions.
- The other DT frequencies (120..200 Hz) are not used.
