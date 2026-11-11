# StarbiteRTC: register facts and sources

Labels: **[V]** checked by us; **[S]** a third party says so; **[I]** our inference.

Sources:
- **RTCH** = `ref/starlte-kernel/include/linux/mfd/samsung/rtc-s2mp.h` (Samsung GPL header, register numbers and bit
  positions only).
- **REFPORT** = an earlier port's `drivers/power/s2mps18_rtc.c` (APSL-2.0 per an earlier port's `LICENSES.md` row for its
  `*` files; Claudius for an earlier port). It cites Samsung's `rtc-s2mps18.c`, `s2mps18_core.c`, the S9 defconfig and
  S-Boot's log. Facts used, no code copied. **Credit: an earlier port project (Claudius for an earlier port), APSL-2.0.**
- **MAINLINE** = Linux `drivers/rtc/rtc-s5m.c` for the S2MPS13/14/15 (from memory, not re-read here).
- **ACPM** = ../StarbiteACPM/NOTES.md (the transport: PMIC type 2 on ACPM channel 2).

## The block

| Fact | Value | Source |
|---|---|---|
| Transport | ACPM PMIC request, type 2 ("i2c address 0x02" of the S2MPS18) | REFPORT [S]; framing [V 0302] |
| Layout | the one **without** `CONFIG_RTC_DRV_S2MP` (with a MSEC register), since the S9 defconfig has `CONFIG_RTC_DRV_S2MPS18=y` and no `CONFIG_RTC_DRV_S2MP` | RTCH `#else` branch [V header], defconfig per REFPORT [S] |
| 0x00 CTRL | bit 0 BCD_EN, bit 1 MODEL24 | RTCH [V] |
| 0x01 WTSR_SMPL | WTSR / SMPL timers and enables (power-loss restart) - **never written** | RTCH [V] |
| 0x02 UPDATE | bit 0 RUDR, bit 1 WUDR (`_REV`), bit 2 FREEZE, bit 4 AUDR (`_REV`) | RTCH [V]: only the `_REV` positions are defined in the S9's header |
| 0x03 CAPSEL | [7:4] = 0xF once set up (Linux's test); oscillator load - **never written** | REFPORT [S] |
| 0x04 MSEC | not decoded (coding unknown) | RTCH [V], [I] |
| 0x05 SEC, 0x06 MIN | [6:0] | RTCH [V], REFPORT masks [S] |
| 0x07 HOUR | [4:0] binary (BCD needs [5:0]), bit 6 PM | RTCH `HOUR_PM_SHIFT 6` [V], REFPORT mask 0x1f [S] |
| 0x08 WEEK | one bit per day (`1 << wday`) | REFPORT [S] |
| 0x09 DAY, 0x0a MONTH (1-12), 0x0b YEAR (since 2000) | | REFPORT [S] |
| 0x0c..0x19 alarms 0/1, 0x1a OSCCTRL | **never written** | RTCH [V] |
| S-Boot | reads the clock every boot, CTRL = 0x02 (binary, 24 h) | S-Boot log per REFPORT [S] |
| The phone's clock today | read 2018-01-01: reset and never set (Android sets it from the network) | REFPORT (runs/auto-210855) [S] |

## Sequences

- **Read:** set RUDR in UPDATE (the register read first, our bit added), wait for it to clear (up to 50 x 1 ms),
  then read 0x00..0x0b. MAINLINE sets RUDR and sleeps 1-5 ms without polling [S]; whether the S2MPS18 clears RUDR
  by itself is **[I]**: if not, the driver still reads and logs "RUDR did not clear" (the registers may then be an
  older latch). REFPORT reads without latching (it gets S-Boot's latch and adds uptime) [S].
- **Write:** SEC..YEAR written in the coding CTRL says, PM bit set for hours >= 12 as the vendor driver does [I],
  WEEK = `1 << wday`; then WUDR set and polled until it clears; then a latched read-back must be within 3 s of what
  was written. MAINLINE polls WUDR's auto-clear on S2MPS14/15 [S]. The bit position (bit 1, `_REV`) is **[I]**:
  RTCH only defines `_REV`; MAINLINE's S2MPS14 uses bit 4 for WUDR and bit 1 for AUDR.
- **Validity:** MODEL24 set, CAPSEL[7:4] = 0xF, every field in range, day within the month, year 2024..2099.
  Otherwise the clock is "not set" and `getGMTTimeOfDay` returns 0 (the calendar then starts as today: card clock
  0393, or 1970).
- **Never written by us:** CTRL (so a 12-hour or reset clock is read-only), WTSR_SMPL, CAPSEL, MSEC, alarms,
  OSCCTRL. Enforced twice: hw/ never issues them, and StarbiteACPM's policy refuses them.

## Hardware-safety notes

- If WUDR were really at bit 4 and AUDR at bit 1 (MAINLINE's S2MPS14 layout), our "WUDR" would latch the alarm
  registers instead: the time would not change (the read-back check fails, logged, no retry loop), and alarm 0/1
  could take whatever their registers hold. With `CONFIG_RTC_ALARM_BOOT` an enabled alarm can power the phone on
  at that time. Low risk; this is why writes are off by default (`sbrtcwrite=1`) and M5b checks a single write first.
- No regulator, no SMPL/WTSR (which decide whether the phone restarts after a power glitch), no oscillator setting
  is ever written.

## Open questions

1. Does RUDR self-clear on the S2MPS18? (M5a logs it.)
2. WUDR at bit 1 (`_REV`)? (M5b: one write, read back.)
3. MSEC's coding (for sub-second `getUTCTimeOfDay`; not needed).
4. Should the RTC hold UTC (Apple, Linux) or local time (Android writes UTC via the kernel; Samsung's S-Boot shows
   it as is)? We write UTC.
