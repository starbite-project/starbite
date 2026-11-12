# Starbite OS: proximity, light, pressure, gyroscope, and the small status parts

Drivers track, 2026-11-12. Written and host-tested. **Nothing in this document has run on the phone yet.**

Files: `starbite/sbos/kern/shub_more.c` (the hub's extra sensors and the probe), `senstat.c` (hall, wireless
charger, heart-rate ID), `shub_more.h` (the API for both). Host tests: `kern/host/shub-more-test.sh` (the Mac) and
the extended `starbite/tools/shub-sim-test.c` (run by `build.sh` in the VM, with the real hub patch).

## 1. The hub's sensors

### 1.1 Wiring

None of these parts is on an AP bus. They sit behind the sensor hub MCU inside the BCM4775 GNSS chip
(docs/SENSOR-HUB.md). `kern/shub.c` starts the hub, loads its firmware from the card (`/STARBITE/FW/BCM4775.BIN`)
and runs the accelerometer, magnetometer and step counter. This driver asks the running hub for more.

| Sensor | Part | Samsung number | Period here |
|---|---|---|---|
| Proximity | AMS TMD4906 | 7 | 200 ms |
| Light | AMS TMD4906 | 9 | 200 ms |
| Pressure | ST LPS22H | 5 | 200 ms |
| Gyroscope | ST LSM6DSL | 1 | 100 ms |

Parts: starlte defconfig `CONFIG_SENSORS_SSP_TMD4906`, `_LPS22H`, `_LSM6DSL`. Numbers: `sensor_list.h`. Periods:
what an earlier port used on this phone (an earlier port `ssp.c`, facts only).

### 1.2 Commands and sample layouts

Commands (Samsung GPL `ssp_i2c.c` `send_instruction`, `ssp_sysfs.c`): enable = SSP WRITE `0xA1`
`[type][s32 period ms][s32 max latency 0][s8 0]`; disable = `0xA2` `[type][s32 period ms]`. Both go through
the same RPC 0x20 wrapper and echo handshake as shub.c's own start.

Samples arrive as `0x37 [type][sample][u64 ns]` records (`ssp_data.c`, `ssp.h` `struct sensor_value`, with
starlte's defconfig: `_LIGHT_REPORT_LUX` on, `_LIGHT_MAX_GAIN_2BYTE` off, TMD4906 is not TMG399x):

| Type | Bytes | Layout | Units |
|---|---|---|---|
| 7 proximity | 3 | u8 detect, u16 adc | detect 1 = near (`ssp_input.c` reports `!detect` as the distance) |
| 9 light | 18 | u32 lux, s32 cct, u16 r g b w, u8 a_time, u8 a_gain | lux as the hub computes it |
| 5 pressure | 6 | s32 pressure, s16 temperature | temperature 0.01 C (`pressure_lps22h.c` prints it /100). Pressure unit: see below |
| 1 gyroscope | 12 | s32 x, y, z | 70 mdps a count [GUESS] |

`shub_proto.c` now also knows the lengths of types 2, 10, 15, 24, 25, 29, 30, 31 and 35, so a frame that carries
one of them is no longer cut short.

**Pressure unit.** The GPL source does not state it. The LPS22H's own unit is 1/4096 hPa; an earlier port's notes say
hundredths of hPa. The driver accepts both, told apart by the plausible range 300..1100 hPa (1.2M..4.5M vs
30000..110000: no overlap). The probe logs which one the phone sends.

**Gyroscope scale.** Samsung's default `uGyroDps` is 1000 (`ssp_dev.c`), which `ssp_input.c` passes through with no
shift. 70 mdps a count is the LSM6DSL's 2000 dps sensitivity (`factory/gyro_lsm6dsl.c` `DEF_GYRO_SENS`). To
verify: turn the phone a quarter turn flat on a table in about one second; the z rate should peak near 90000 mdps.

### 1.3 Power and lifetime

- A sensor runs only while asked for. Every getter is a keep-alive for its sensor. After 10 s without one the
  driver removes it at the hub.
- `sens_incall(1)` pins the proximity sensor (200 ms) until `sens_incall(0)`.
- The hub is started and kept running through `surv_sensors_on()` (survival.c), the Compass page's way. Its own
  60 s idle stop applies again once nothing here is wanted. A failed start is retried after 30 s at most.
- A new start of the hub (shub.c's `shub_starts()` count) turns the wanted sensors on again.
- A sensor that sends nothing for 3 s after enabling is asked again, up to three times.
- Last 2 %: survival refuses the hub; `sens_want` returns -1 and `sens_why()` says so.

### 1.4 API (`shub_more.h`)

| Call | Returns |
|---|---|
| `sens_prox_near()` | -1 unknown, 0 far, 1 near |
| `sens_lux(&lux)` | 1 valid |
| `sens_pressure_hpa(&hpa_x100, &temp_x100)` | 1 valid (fresh within 2 s) |
| `sens_gyro_mdps(xyz)` | 1 valid (fresh within 2 s) |
| `sens_incall(on)`, `sens_incall_screen_off()` | the in-call screen: 1 = screen and touch off now |
| `sens_auto_brightness()` | a backlight level 0..255 from the light, -1 unknown (6 at 0 lux, 10 at 1, 40 at 10, 90 at 100, 170 at 1000, 255 from 10000) |
| `sens_want(mask)`, `sens_release(mask)` | ask / stop explicitly |
| `sens_why()` | plain words for the UI |

Proximity and light are on-change sensors on Android. Their value stays valid while the sensor is on and one sample
has come; pressure and gyroscope need a sample younger than 2 s.

### 1.5 What changed in shub.c (owned by the survival session)

All additive:

- `shub.h`: `shub_enable_type(type, period_ms, on)`, `shub_set_hooks(sample, poll)`, `shub_starts()`.
- `shub.c`: a 3-field struct `X` (the two hooks and a start counter); `sample()` passes the raw sample of any
  sensor other than 0 / 4 / 18 to the hook; `shub_poll()` frees the one SSP request slot after the echo (or 1 s)
  and calls the poll hook at its end; `++X.starts` where the hub reaches "running"; the three new functions.
- `shub_proto.h/.c`: `struct shp_rec` gains `raw` and `raw_len`; `sample_len` knows 11 more sensor types.

Without a hook set, shub.c behaves exactly as before.

## 2. Status parts (`senstat.c`), read only

| Part | Wiring | What is read |
|---|---|---|
| Flip-cover hall | gpa3-6, ALIVE 0x14050080, DAT bit 6, EINT, pull-down | the pin: high = no magnet (Utility OS 2 on the phone, 2026-11-07) |
| Certify hall | gpa0-2, ALIVE 0x14050020, DAT bit 2 | the pin |
| P9320 wireless charger | `idt,mfc-charger` @ 0x3B on hsi2c@14370000 (I2C_CMGP03), pins gpm16-0 / gpm17-0 function 3; wpc_det gpa2-0, wpc_int gpa3-1 | chip ID 0x00/01 (0x9320), rev 0x02, status 0x20, Vout ADC 0x3C, Vrect 0x40, die temp 0x46, frequency 0x48 (Samsung GPL `mfc_charger.h`) |
| MAX86915 heart rate | `hrmsensor` @ 0x57 on hsi2c_20 0x108E0000; VDD S2MPB02 LDO8, pull-ups S2MPS18 LDO38 | LDO38 (PMIC 0x6F, read); the part ID 0xFF only if LDO38 is on and the bus is already up |
| NFC | `sec-nfc` @ 0x27 on hsi2c_20, VEN expander P1_3, FIRM P1_1, PVDD LDO38, clock PMU CLKOUT0 | recorded only |
| Fingerprint | Egis ET510 on spi_1 0x10450000, `secure-mode` (TEE) | recorded only; never touched |

The P9320 is powered by the pad's coil: off a pad it does not answer, which is normal. Nobody else uses its bus.
`sens_wpc_read` checks the three I2C_CMGP03 gates (CMU_CMGP +0x2044, +0x2048, +0x2060; refuses if one is held
closed, never opens one), saves the two pins and 12 controller registers, sets the pins to function 3, brings the
controller up (S-Boot's timing if it left one, else Android's 200 MHz / 400 kHz one), reads, and writes everything
back. Vout in mV = raw x 12600 / 4095 is a [GUESS] (12-bit ADC); the raw value is logged too.

The heart-rate sensor needs two regulator writes (LDO38 and LDO8) before it answers. This driver writes no
regulator, so in practice it reports "rails off". Lighting its LEDs for a heart-rate reading was left out.

## 3. The probe

`int drv_sens_probe(int level, unsigned *wait_ms);` `void drv_sens_abort(void);`

| Level | Budget | Touches | Pass line |
|---|---|---|---|
| 0 | 2000 ms | reads only: PMU CHUB status, gph0 (only if CHUB is on), HOST_REQ / AP_SLEEP / NSTDBY / SHUB_INT pins, LDO37, both hall pins, wpc_det / wpc_int, the I2C_CMGP03 gates, controller and pins, LDO38. The P9320's ID only if its bus is already set up (it never is today) | `drvprobe: sens: PASS cover open, no wireless pad, CHUB on, HRM rails off` |
| 1 | 25000 ms | one P9320 look (pins + controller, put back); the HRM ID if its rails are on; then the hub: started from the card as the Compass page does (or used if it already runs), proximity + light + pressure + gyroscope for 6 s, min / max / mean per type, all four removed, the hub left (if the probe started it) | `drvprobe: sens: PASS proximity far, light mean N lux, N hPa, gyroscope N samples; all four off again` |
| 2 | 100 ms | nothing | `drvprobe: sens: PASS nothing to see at level 2 ...` |

Fail lines: `FAIL a status register did not read (...)`, `FAIL the sensor hub's firmware is not on the card:
nothing measured` (after `the hub's firmware is not on the card (/STARBITE/FW/BCM4775.BIN): ending`),
`FAIL no samples from: <names>`, `FAIL did not finish within its budget: everything put back`, or shub.c's own reason
when the hub does not start. The abort puts the hub in standby (if the probe started it), which stops every sensor
and restores every register (shub_leave).

Statistics lines, level 1: `drvprobe: sens: prox adc n .. min .. max .. mean ..; near ...; lux ...`,
`drvprobe: sens: hPa ...; temp C ...; pressure unit 1/4096 hPa`, `drvprobe: sens: gyro mdps x ...; y ...; z ...`.

## 4. Risks and what is left

- The commands to enable a fourth or later sensor while the hub runs follow Samsung's layout; shub.c's own start
  used the same one for three sensors on the phone. The disable command has not been seen on any phone.
- The pressure unit and the gyroscope scale need one look on the phone (the probe logs both).
- The proximity's "near" threshold is the hub's (Samsung's `prox_thresh` factory values are not sent here). If the
  hub uses an uncalibrated default, near might trigger late or early. Look at the adc values the probe logs.
- The P9320 bus has never been driven by us. A wrong timing can only make the read fail; it writes nothing to the
  chip.
- Not wired: who calls `sens_incall()` (the modem / call screen) and `sens_auto_brightness()` (the UI). The getters
  are not system calls yet (app.c).
