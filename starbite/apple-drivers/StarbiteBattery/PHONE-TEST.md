# StarbiteBattery: phone tests (milestone M11: the battery percentage changes under load; charging holds)

**Question:** does IOPMPowerSource follow the MAX77705's real state of charge, and does it move when the phone
works hard?

The only MAX77705 write is the charger watchdog clear (CNFG_06 WDTCLR = 01 every 30 s); no PMIC, no other charger
register. The other bus writes go to the HSI2C controller (reset, S-Boot's own timing restored). The load is every CPU busy for 40 s at the clocks already in use (no DVFS request).

## Prerequisites

- M1 passed (arm-io nubs from the board ADT); the node is named `hsi2c-pmic` (board block, unchanged).
- Kernel: M1 kernel + StarbiteBattery (INTEGRATION.md), SweetBattery's personality removed for this kernel.
- Phone **unplugged** (on battery), so the current is a discharge and must grow under load. Battery above 20 %.
- Dry run, verified backup, as always.

## Catalog row and boot

Hidden row `applem11bat`, `wdt = 82`, `wdt_feed = "xnu"` (as StarbiteACPM's PHONE-TEST.md).
Boot-args: `tools/cmdline/apple-m1.txt` with `sbstop=pe` replaced by

```
sbstop=battery sbcrash=recovery sbbatpoll=2 sbbatload=35
```

Run: `store-flash.sh ... && boot-safe.sh applem11bat`.

## Dead-man (independent of the driver)

- `sbstop=battery`: kernel 0423 never feeds the loader's 82 s watchdog and sets INFORM3 = recovery (0422): any
  hang, panic or a load that starves the system returns the phone to TWRP within 82 s of the hand-off.
- The driver's own stop: right after the verdict line it calls `starlte_sbstop_now("battery")` (pmsg dump, reset
  into TWRP).
- Timing: driver start ~10-20 s after hand-off, 10 s baseline, 35 s load, verdict 2 s later: ~57-67 s, inside 82 s.
  The load threads end by themselves at 35 s.

## Expected pmsg lines

```
StarbiteBattery: HSI2C 0x14360000, timing FS1 0x... FS2 0x... FS3 0x... (S-Boot's), MAX77705 PMICID 0x..
StarbiteBattery: t=.. s MAX77705: 74.3 % (74%) 3912 mV -280 mA (avg -260) 26.0 C 2190/2950 mAh cycles . ext 0 chg 0
    full 0 dtls 8/3/0; bus errors 0 fallbacks 0; measured; temperature is S-Boot's last write (not live)
   (every 2 s)
StarbiteBattery: M11 load: 8 busy threads for 35 s
StarbiteBattery: t=.. s MAX77705: 74.2 % ... -1450 mA ...
StarbiteBattery: M11 PASS: measured yes, SOC 74.3 % -> 74.0 % (range 740..743 x0.1 %), current -280 mA idle ->
    -1520 mA under load (rose), percent shown 74%
STARBITE: sbstop=battery: stage reached, resetting into TWRP
```

## Pass line

`StarbiteBattery: M11 PASS` (every sample measured, the 0.1 % state of charge moved during the test, the discharge
current grew by at least 200 mA under load). The whole-percent value may or may not change in 35 s (about 0.5 %
expected at ~1.5 A); the pass is on the gauge's 0.1 % value, which is what powerd's percentage is computed from.

## If it fails

- `fallback` lines: read `StarbiteBatteryNote`'s reason in the line (gauge not answering / model not loaded / bus
  errors). No harm: the static 100 % is what SweetBattery shows today.
- `current did not rise`: the load threads did not run (look for the load line) or the phone was plugged in.
- `CHARGER WATCHDOG EXPIRED` in a sample: expected after some minutes on a cable (NOTES.md); not this test's
  question.

---

# M11b: charging continues past the old cut-off (charger watchdog clear)

**Question:** with the clear every 30 s, does the charger keep charging on the cable for 10 minutes, where without
it CHG_DTLS turns 0x0b (watchdog expired) and charging stops?

Too long for an 82 s self-stop, so the dead-man is today's kernel's 0421 one: a timed boot.

- Row `applem11chg`: `wdt = 60`, `wdt_feed = "xnu"`; phone **on the USB cable** (the Mac), battery below 90 % (so it
  is not "full").
- Boot-args: `tools/cmdline/apple-m1.txt` with `sbstop=pe` removed, plus
  `sweetreboot=660 sbnoui=0 sbcrash=recovery sbbatpoll=10 sbchgtest=600`.
- **Dead-man (independent of this driver):** 0399/0421 in today's kernel feed the loader's 60 s watchdog only
  until sweetreboot + 60 = 720 s and stop at once on a panic; 0422 sets INFORM3 = recovery; the sweetreboot timer
  itself resets into TWRP at 660 s. Any hang ends in TWRP by about 780 s. StarbiteBattery does not feed anything.

Two boots, control first:

1. **Control (`sbchgwdt=0`)**: the old behaviour, read only. Expected: samples `ext 1 chg 1 ... dtls 1/3/3`, then at
   some point `dtls 11/...` and `CHARGER WATCHDOG EXPIRED`; `StarbiteBattery: M11b FAIL: charging lost at T s
   (CHG_DTLS 11)`. T is the old cut-off (the timeout we never knew). If it never expires in 600 s, the clear is
   moot and boot 2 only checks that it does no harm.
2. **With the clear (default)**: expected `StarbiteBattery: charger watchdog: cleared every 30 s (CNFG_06 0xbd =
   0x0d: WDTCLR 01, CHGPROT kept)` once, samples charging throughout.

**Pass line** (boot 2): `StarbiteBattery: M11b PASS: on the cable 600 s, charging throughout, 20 watchdog clears,
never expired`, with boot 1 having shown the cut-off earlier than 600 s.

Afterwards I read the battery temperature in TWRP (`/sys/class/power_supply/battery/temp`): it must be well under
the 45 C Engineer OS uses for charge tests.
