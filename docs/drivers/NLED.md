# Notification LED (MAX77705 RGB block)

Driver: `starbite/sbos/kern/nled.c`, `nled.h`. Host test: `starbite/sbos/kern/host/nled-test.sh`.
Status: written 2026-11-12. **Not run on a phone yet.**

## Wiring

| What | Value | Source |
|---|---|---|
| Bus | HSI2C 0x14360000 (USI_CMGP03), brought up by libsb `max77705.c` | libsb |
| Chip address | **7-bit 0x66** (the MAX77705's top / PMIC address) | Samsung `drivers/leds/leds-max77705-rgb.c`: `max77705_rgb->i2c = max77705_dev->i2c`; `drivers/mfd/max77705.c`: `I2C_ADDR_PMIC (0xCC >> 1)` |
| Registers | LEDEN 0x30, LED0BRT..LED3BRT 0x31..0x34, LEDRMP 0x36, LEDBLNK 0x38 | `include/linux/mfd/max77705-private.h:248-257` |
| Channels | LED0 white, LED1 red, LED2 green, LED3 blue | `enum max77705_led_color { WHITE, RED, GREEN, BLUE }` |
| LEDEN | 2 bits per LED at bit 2n: 0 off, 1 on, 2 blink | `max77705_rgb_set_state`: `led_state << (2*n)`, mask `0x3 << 2*n` |
| LEDBLNK | on time [7:4], off time [3:0] | Samsung `LEDBLNK_ON` / `LEDBLNK_OFF` (copied as `nled_blnk_code`) |
| LEDRMP | ramp up [7:4], ramp down [3:0] | `max77705_rgb_ramp` (we always write 0: no ramp) |
| Current | LEDxBRT, 0.1 mA a step | Samsung's DT values below |

**About 0x4A.** `max77705-private.h` says "Slave addr = 0x94: RGB LED" (7-bit 0x4A). That comment is left over
from older Maxim parts. Samsung's MFD driver for this phone creates no client at 0x4A. The RGB driver uses the
parent's client at 0x66. (8-bit 0x4A is the USB-C / MUIC block, 7-bit 0x25.) The Samsung `.c` files are not in our
sparse ref checkout. They were read from the public GPL tree `android_kernel_samsung_exynos9810`
(commit e5906667068e), for facts only.

## Current caps

The phone's DT node `rgb` (`device/dtb/starlte.dts:12530`) gives `normal_powermode_current = 10` and, for the
default colour set "uu", `br_ratio_r = 700`, `br_ratio_g = 60`, `br_ratio_b = 120`. Samsung's `set_state` writes
`brightness * ratio / 100`. Its patterns pass `led_dynamic_current` (= 10), so Samsung's full brightness is:

| LED | Cap (BRT code) | Current |
|---|---|---|
| LED0 white | 10 | 1.0 mA |
| LED1 red | 70 | 7.0 mA |
| LED2 green | 6 | 0.6 mA |
| LED3 blue | 12 | 1.2 mA |

`nled.c` never writes a BRT value above these caps. Colour 255 at 100 % equals the cap. The other colour variants
in the DT have the same ratios or higher, so "uu" is the safe choice.

The one exception is the restore: it puts back exactly what the block held when we first read it, even if another
OS left a higher value.

## What is written, and how

- Only the seven LED registers at 0x66. `nled_allowed()` refuses everything else before the gate is asked. That
  includes MAINCTRL1, the haptic registers, INTSRC, BOOST and SW_RESET 0x50 at the same address, and the charger,
  gauge and USB-C.
- Every write goes through libsb's gate: `sb_max_write(0x66, reg, val, SB_TIER1)` under a tier-1 authorisation that
  covers exactly that one register. This is the same pattern `kern/vib.c` uses for the haptic bits at this address.
  **No libsb change was needed.** The gate already covers any MAX77705 address (`SB_SP_MAX`, `addr7 << 8 | reg`).
- Every write is read back. A NACK, a read-back mismatch or a gate refusal turns the LED off (LEDEN 0) and sets
  `nled_why()`.
- A register that already holds the value is not written again.
- The LED blinks on the chip's own timer, so the AP can sleep.

## API (nled.h)

- `nled_init()` reads the block once and keeps it "as found". A failed read is retried on the next call.
- `nled_color(r, g, b, pct)` sets a steady colour. `nled_blink(r, g, b, pct, on_ms, off_ms)` blinks it. `on_ms` runs
  100..3250 and `off_ms` 500..12000, both rounded to the chip's steps.
- `nled_off()`, `nled_restore()`, `nled_why()`.
- `nled_pattern(p)`, with `nled_pattern_name(p)` for the UI:

| Pattern | Colour | Timing | Samsung pattern |
|---|---|---|---|
| `NLED_MISSED_CALL` "Missed call" | blue | 0.5 s on, 5 s off | MISSED_NOTI |
| `NLED_MESSAGE` "New message" | blue | 0.5 s on, 5 s off | MISSED_NOTI |
| `NLED_CHARGING` "Charging" | red | steady | CHARGING |
| `NLED_CHARGED` "Charged" | green | steady | FULLY_CHARGED |
| `NLED_LOW_BATTERY` "Low battery" | red | 0.5 s on, 5 s off | LOW_BATTERY |
| `NLED_CHARGE_ERROR` "Charging problem" | red | 0.5 s on, 0.5 s off | CHARGING_ERR |

All patterns run at the caps (Samsung's own brightness). The OS decides which pattern wins. For example, a missed
call over charging is the caller's choice, and only one pattern shows at a time.

## Probe

`int drv_nled_probe(int level, unsigned *wait_ms); void drv_nled_abort(void);` Budget: 12000 ms
(`NLED_PROBE_BUDGET_MS`).

- **Level 0** (read only): reads the seven registers and logs them.
  PASS `the LED block answers at 0x66, LEDEN 0x00 (all off)`. FAIL if the block does not answer.
- **Level 1**: there is nothing silent to do. PASS `skipped (attended)`.
- **Level 2** (attended): each step lasts 1 s at a quarter of the cap: red, then green, then blue, then white
  (red + green + blue), then LED0 (the DT's `led_w`). Then blue blinks 0.5 s on, 0.5 s off for 3 s. Then the block is
  put back as found and read again. About 8 s in all. Before each step it logs
  `drvprobe: nled: now showing <colour>`. **The person watching says which colour lit at each step.** This is the
  first check of the channel order and of the LEDEN bit meanings.
- Abort, a failed step, or overrunning the 12 s budget each put the block back as found.

## Risks and open points

- **Never run on the phone.** Three things come from Samsung's driver and have not been seen here: the address
  (0x66), the channel order and the LEDEN meanings. Level 2 checks all three.
- LED0 ("led_w") may not be fitted on the S9. If nothing lights at that step, that is expected.
- The colours are not calibrated. The ratios only make Samsung's default colours look right.
- Each write logs one `max77705 write` line from libsb. A pattern change makes up to six lines.

## Host test

`sh starbite/sbos/kern/host/nled-test.sh`: 46 scenarios, each in its own process. It checks:

- the allow-list over six addresses × 256 registers, the caps and the blink codes;
- that levels 0 and 1 write nothing;
- level 2's sequence, values and timing;
- an abort after every step, and a budget overrun;
- a NACK at each of the 19 writes, a stuck register (one at a time), and a gate refusal;
- the API and the patterns;
- a block found above the caps (put back exactly);
- a bus that answers only later.

The fake checks every write on its own: it must be an LED register under an open tier-1 authorisation that covers
just that register, and within the caps. `nm` checks that `nled.c` calls no other writer.
