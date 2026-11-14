# Iris unlock for Starbite OS: the plan (2026-11-14, drivers track, off the phone)

Goal (the user's words): "a nice and fast iris scanner so it feels just like apple's faceid". Target: the phone
unlocked under ~0.5 s from wake, from the user simply looking at it.

Status: **plan only**. Nothing has run on the phone. Every phone step below is attended (the user present), and
never touches EFS / CPEFS / PARAM. Facts are from this phone's device tree (`build/linux-bundle/lts.dts`) and Samsung's
GPL kernel for it (LineageOS `android_kernel_samsung_exynos9810`, lineage-20; files named below); an earlier port's iris
capture driver (`build/refport27/refport-kernel/drivers/camera/exynos/exynos_iris.c`, APSL) is read as facts
only, never copied.

## 1. The hardware path

| Part | What | Source |
|---|---|---|
| Sensor | Samsung **S5K5F1** ("STAR IRIS Sensor", internal id 306), I2C **0x10** on `hsi2c@108D0000` (I2C ch 6, USI07's second channel, pins gpp4-6/7), 16-bit registers | DT `fimc-is-secure@10`; `fimc-is-device-module-5f1.c` |
| Mode | one: **2400 x 2400 RAW8, 30 fps**, 2 MIPI lanes at 2041 Mb/s, VC0. Line length 0x0A38, frame length 0x13D7; 26 MHz MCLK | `config_5f1`, `setfile_vision_5f1_fps_30_26MHz` |
| Receiver | **CSIS3** (0x16260000), DMA ch 3 (0x162E0300), behind its own SysMMU, ISPPRE power domain (as the front camera's CSIS1) | DT `fimc_is_sensor@16260000`, scenario 6 |
| Supplies | S2MPS18 (the main PMIC, ACPM): **LDO41** VDDIO 1.8 V, **LDO40** VDDA 2.8 V, **LDO39** VDDD 1.05 V | DT regulators |
| GPIOs | `gpio_iris_en` (bank phandle 0x4d pin 7), `gpio_reset` (0x7e pin 3), MCLK on mclk_ch 2 (0x7d pin 6) | DT |
| IR LED | on the **S2MPB02** (camera PMIC, 0x59 on USI10, the same chip as the torch: `kern/fled.c`, `cam.c`): FLED_CTRL2 0x13, CUR2 0x16 (high nibble = IR current code), TIME2 0x17 (bit 0 = the IR max-time timer), IRON1/2 0x18/0x19 (10-bit pulse width; IRON2 [4:0] = max on-time), IRD1/2 0x1A/0x1B (10-bit delay; IRD2 bit 1 active discharge) | `leds-s2mpb02.c` |
| LED trigger | not in the kernel: no GPIO or I2C enable per frame. CTRL2 "GPIO polarity" mode = most likely a hardware strobe from the sensor (unconfirmed) | as above |

Stock power-on order (`sensor_5f1_power_setpin`): iris_en high; VDDIO on; VDDA on; VDDD 1.05 V on, 1 ms; I2C pins
on, 10 us; reset high; MCLK on, 2 ms. Off: MCLK off (200 us), reset low, I2C off, VDDD / VDDA / VDDIO off, iris_en
low. Init: 0xFCFC = 0x4000, 0x6010 = 1 (reset), 5 ms, 0x6214 = 0x7971, 0x6218 = 0x7150, then the ~50-write setfile.
Identification: 0x0002 reads **0xA1** on a final module (there is no chip-ID register check in Samsung's driver);
OTP page 0x3E00 / 0x3F00 via 0x0A02 / 0x0A00, data 0x0A04-0x0A09. Stream: 0x0100 = 1 / 0 (+40 ms). Exposure
0x0202 (lines), analogue gain 0x0204 (0x20 = 1x).

What we already have: `kern/cam.c` brings up ISPPRE, its clocks, the DTZPC save/restore SMC, the D-PHY and CSIS1's
DMA for the front camera (PASS on the phone). Iris is the same chain on CSIS3 with a different D-PHY, a faster lane
(2041 Mb/s: a new settle value from `fimc-is-hw-settle-10nm-lpp.h`) and the main PMIC's LDOs instead of the
S2MPB02's. `libsb` has `sb_pmic_write` (tiered) for LDO39-41.

Known risk on the bus: USI07's *main* HSI2C (the speaker amps' 0x108C0000) never left its START state on this phone;
the amps work through a bit-banged bus (`amp.c amp_bb_xfer`). The iris bus is USI07's *second* controller
(0x108D0000): it may work, or need the same fallback on gpp4-6/7.

## 2. The decisive unknown: is CSIS3 ours?

On stock, CSIS3 is Samsung's **secure camera** channel (`SECURE_CAMERA_CH = CSI_ID_D`): with scenario 6 the kernel
calls `exynos_smc(MC_SECURE_CAMERA_PREPARE 0x83000043)` and frames land only in the `secure_camera` ION heap
(0xE1900000, 30 MiB, `ion,untouchable`), which the normal world cannot map; matching happens in the TEE (Trustonic
Kinibi). The GPL code has nothing that locks CSIS3 *outside* the secure scenario; whether the secure firmware's
TZASC / TZPC already guards CSIS3 or its DMA at boot can only be tested on the phone (probe P1b). If it is locked,
there is no iris camera for us (the PREPARE path needs Samsung's TA) and this plan stops at a clear "not possible".

## 3. Our own matcher (stock's is in the TEE: we cannot use it)

A classic Daugman pipeline, chosen because it is fast, small, well understood and needs no training data:

1. **Capture.** IR-lit frames with fixed exposure (the LED is the light: no auto-exposure loop). Both eyes in view at
   25-35 cm. A frame is 5.76 MB (RAW8).
2. **Find the eyes.** Downsample 4x (600 x 600); under IR the pupils are the darkest round blobs: threshold +
   connected components + a circle fit; the LED's corneal reflection (a small saturated spot inside each pupil)
   confirms an eye and its position.
3. **Segment** each eye at full resolution (~300 x 300 crop): pupil boundary and limbus by Daugman's
   integro-differential operator (or a radial-gradient search); eyelids as parabolas; mask eyelashes and the LED's
   reflection.
4. **Normalise** to a fixed polar grid (rubber sheet, e.g. 64 radial x 512 angular).
5. **Encode** with 2D Gabor / log-Gabor phase quantisation: a 2048-bit iris code + a 2048-bit mask per eye (512 bytes).
6. **Match**: fractional Hamming distance over the unmasked bits with circular shifts (head tilt, +-12 steps);
   threshold ~0.32 (Daugman: FAR around 1e-6 per eye). Both eyes: accept on the better eye, with the other one's
   distance as a second check. Microseconds of work.
7. **Enrolment**: 15-20 frames per eye while the user looks at the screen at two distances; quality gates (focus
   by high-frequency energy, occlusion < 35 %, pupil/iris ratio in range); keep the best 4 codes per eye.
8. **Templates**: ~4 KiB in all, in the state slot (A/B, already on the card) as their own record. Never exported.
   Deleted by "Reset" / passcode change.

**Liveness** (Face ID's "attention", in iris form): the corneal reflection must be a sharp spot on a curved surface
in both eyes at the right separation; a frame-to-frame pupil response to a change in the LED's current; small
natural eye movement across 2-3 frames. A printed iris has no 3D corneal reflection and no pupil response.

**Security honesty.** Without a TEE, this is a convenience unlock, not a vault: code running with kernel rights on
Starbite could read or swap templates. So: the passcode stays the root of trust (required after a restart, after 5
failed iris tries, after 48 h, and to change any security setting), as on an iPhone.

## 4. How fast: the 0.5 s budget from wake

| Step | Time | How |
|---|---|---|
| Rails, reset, MCLK | ~4 ms | stock delays (1 ms + 2 ms + ...) |
| Sensor init (setfile, ~55 writes) | ~8 ms at 400 kHz I2C (~50 ms if bit-banged) | |
| First usable frame | ~35-70 ms | 30 fps; fixed exposure, so frame 1 or 2 |
| Eye find + segment + encode (2 eyes) | ~25-40 ms | one A55 core at full clock with NEON; the big cores if Linux is not using them |
| Match | < 1 ms | |
| **Total** | **~0.1-0.2 s** (bit-banged: ~0.25 s) | |

To feel like Face ID: start the sensor at the wake event itself (Power key, double tap, lift), not at the swipe; show
the padlock opening on the lock screen as soon as it matches; a swipe then goes straight in. If no eye is found in
1.5 s the sensor and LED go off (power and eye safety) until the next wake or a tap on the padlock.

## 5. Eye safety (before any LED light)

The kernel enforces **no** limits: the stock HAL passes width / delay / current / on-time straight to the PMIC, and
the only cut-off is the PMIC's hardware max-on timer. The boot value of CUR2 is the 1050 mA code. Samsung's product
is IEC 62471-certified with *their* per-shot values, which are not in the GPL sources. Our rules:

1. **Measure stock first** (attended, daytime, One UI): read S2MPB02 0x13 / 0x16-0x1B while One UI's iris scanner
   runs (a root shell or a debug read), so our values are never above stock.
2. **Never above stock** in current, pulse width, duty or total on-time per attempt; the PMIC's IR max-time timer is
   always armed (TIME2 bit 0, IRON2 [4:0]); a software budget per unlock (e.g. at most 45 frames = 1.5 s, then off);
   the LED only while the sensor streams and the screen is on; off on any error, timeout, or screen-off.
3. **Start low**: the first phone frames with the LED at the lowest current code and the shortest pulse, raised only
   as far as a usable image needs.

## 6. What to probe on the phone first (in order)

| Probe | Kind | What it settles |
|---|---|---|
| **P0** read-only | silent, unattended OK | LDO39/40/41 state (`sb_pmic_read`), the iris GPIOs' state, USI07_I2C's mode word, S2MPB02 0x13 / 0x16-0x1B (the LED's boot values), whether CSIS3's registers read at all (`sb_probe32`: a fault = locked by the secure world) |
| **P1a** sensor up | attended | the stock power-on order; I2C read of 0x0002 (expect 0xA1) and the OTP page; then everything off. No streaming, no LED. Settles the bus (HSI2C or bit-bang) |
| **P1b** one dark frame | attended | stream one frame **without the LED** into CSIS3's DMA in our own (non-secure) memory: does CSIS3 write it? **The go / no-go for the whole feature** |
| **P2** stock LED values | attended, daytime, One UI | read the S2MPB02 IR registers while One UI's iris unlock runs |
| **P3** one lit frame | attended, eyes at arm's length | the LED at the lowest current, one short pulse, the max timer armed: brightness, corneal reflections visible |
| **P4** offline matcher | Mac | P3's frames (kept on the Mac only, deleted after) drive a host-tested pipeline (eye find, segment, encode, match) |
| **P5** on-phone enrol + unlock | attended | the pipeline in the kernel; timing from wake logged per step |

Code plan: `kern/iris.c` (power, sensor, CSIS3 capture: shares cam.c's ISPPRE / D-PHY helpers), `kern/irled.c` (the
LED with the limits above, its own host test), `kern/irismatch.c` (pure C, host-tested: find, segment, encode,
match), a drvprobe "iris" probe (P0-P1b), and the lock screen's hooks for Track A.
