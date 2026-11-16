# Survival kit (classic Starbite OS, Track D, 2026-11-16)

The main session's 11-16 round of survival features: what each does, where it lives, and what the phone has to show.
Code: `starbite/sbos/kern/survkit.c` (barometer history, altitude, storm trend, the sun, timers, the whistle / siren),
`kern/hrm.c` (the heart-rate sensor), `apps/survkit.c` (the Survival kit app, from Emergency or home), the Knots
chapter in `guide/sbguide.h`, the siren switch in `apps/emergency.c` (Light and sound). Settings keys 80-99
(`sdk/sbapp.h` `SET_KIT_*`; the fire starter keeps 55-79). Host test: `kern/host/survkit-test.c` (in `build.sh`).

| Feature | What it does | Hardware | Seen on a phone |
|---|---|---|---|
| Barometer | Pressure, altitude (the standard atmosphere, or from a typed known altitude), the 1 h / 3 h trend and a chart; opt-in Storm watch: one reading every 15 min, a notification and two buzzes on a fast fall | ST LPS22H behind the BCM4775 hub (the drivers track's `sens_pressure_hpa`; the hub's firmware from the card) | not yet |
| North from the sun | The sun's azimuth and elevation from the place (Emergency > Sun or Android's last fix) and the clock; "face the sun: north is N degrees to your left / right"; a compass check against the sun; the shadow-stick timer | none | not yet |
| Water purification | Timers: boil 1 min (3 min above 2000 m when the barometer knows), chlorine 30 / 60 min, chlorine dioxide 4 h, iodine 30 min, SODIS 6 h / 2 days; a notification and three buzzes at the end, also with the screen off | the motor | not yet |
| Heart rate | A fingertip on the sensor next to the rear camera: the pulse, "steady" when the last five beats agree within 20 %; at most 2 minutes a check; a hand-count guide beside it. Not a medical device | MAX86915 @ 0x57 on HSI2C 0x108E0000; rails S2MPB02 LDO8 (VDD) and S2MPS18 LDO38 (pull-ups), the LED boost pin gpg1-6 | not yet |
| Knots | Nine guide pages: basics, bowline, clove hitch, two half hitches, taut-line hitch, sheet bend, figure-eight loop, trucker's hitch, reef knot | none | n/a |
| Whistle / siren | 3.15 kHz whistle (3 blasts) and a 650-1650 Hz wail at full volume on the loudspeaker; the route and volume put back after | the speaker (snd.c; the classic OS starts it on first use) | not yet |

## The sums (host-tested)

- Altitude: h = 44330.77 (1 - (p / p0)^0.190263) m; the reference p0 from a known altitude by the inverse. A change of
  height reads as pressure (about 1.2 hPa per 10 m): the trend is good only at one height, and the page says so.
- Storm rules: falling 1.5 hPa / 3 h worse weather; 3.5 hPa / 3 h (or 1.5 / 1 h) worse within a day; 6 hPa / 3 h (or
  2.5 / 1 h) a storm likely; rising 1.5 / 3.5 hPa better / clearing.
- The sun: the Astronomical Almanac's low-precision solar position with the hour angle from GMST; within 0.3 degrees
  of NOAA's solar calculator at eight places and times (the test).
- The pulse: the sign turned, an 80 ms smoothing, the slope over 40 ms; a beat where the slope rises through half its
  recent top after falling, at least 300 ms apart, above a noise floor; the median of the last intervals. Synthetic
  fingertip signals at 48, 66, 72, 95 and 150 a minute with baseline drift, a dicrotic notch and noise come out within
  3 a minute; a flat signal gives nothing.

## The heart-rate sensor's bring-up (all put back at the stop)

1. CMU_PERIC1's USI08 gates must be open (refused otherwise; never opened here).
2. LDO38 (L38CTRL 0x6F): if off, on at 1.8 V = 0xEC through the PMIC gate (tier 1), as the touch rails are; refused
   if it holds another voltage. LDO8 (S2MPB02 0x25, via `fled.c`'s `fled_ldo8_*`, which allow only 0x30 / 0xB0 or the
   value found): 0x30 then 0xB0. gpg1-6 output high. 2 ms.
3. USI08 in I2C mode (0x10811034 = 4), gpp5-0/1 function 2 without pulls, the controller with S-Boot's or Android's
   timing. PART_ID 0xFF must read 0x2B.
4. Reset; IR LED 0x46 in the 150 mA range (about 42 mA, pulsed), the others off; IR only; 400 / s with 4 averaged
   (100 a second); polled FIFO every 100 ms; a level control keeps the IR between ~150000 and 420000 counts.
5. Stop (page closed for 3 s, 2 minutes, or an error): the LED off, the chip shut down, the controller, pins, USI
   mode, boost pin, LDO8 and LDO38 written back as found, each logged if it fails.

## Phone checks (S9 #2)

- `starbite-os-survival.img` (unattended, no sound): pressure 300-1100 hPa with the altitude and the sun logged; a
  timer start / stop; the heart-rate sensor 4 s with no finger (the MAX86915 answers, at least 100 samples) and both
  rails back as found.
- `starbite-os.img` (attended, daytime for the sound): the pages, a pulse with a fingertip against a hand count, the
  whistle and siren once each briefly, Storm watch on for an hour.
