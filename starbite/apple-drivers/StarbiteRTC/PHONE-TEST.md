# StarbiteRTC: phone tests (milestone M5: correct date without the card clock)

Three short boots, each answering one question, each ending in TWRP on its own. Only M5b writes anything, and only
the RTC's time registers and UPDATE (never CTRL, WTSR/SMPL, CAPSEL, alarms, never a regulator).

Prerequisites: StarbiteACPM's PHONE-TEST passed (8/8 reads); kernel = M1 kernel + StarbiteACPM + StarbiteRTC
(INTEGRATION.md), bsd/dev ACPM users off (ACPM hazard H1); dry run, verified backup.

Common to all three: a hidden catalog row like ACPM's (`wdt = 82`, `wdt_feed = "xnu"`, files = the M5 image),
boot-args = `tools/cmdline/apple-m1.txt` with `sbstop=pe` replaced as below plus `sbcrash=recovery`; run
`store-flash.sh ... && boot-safe.sh <key>`.

**Dead-man (independent):** `sbstop=` makes kernel 0423 never feed the loader's watchdog (82 s) and sets INFORM3 =
recovery (0422): any hang or panic returns to TWRP within 82 s. The driver also self-stops at its stage. A hung RTC
read costs at most 12 registers x 3 sends x 600 ms = 21.6 s plus 50 ms of latch polling.

## M5a: read (no write)

Boot-args: `... sbstop=rtc`

Expected:
```
StarbiteRTC: S2MPS18 RTC CTRL 0x2 UPDATE 0x0 CAPSEL 0xf. ... YEAR 0x..
StarbiteRTC: clock 20..-..-.. ..:..:.. UTC (...), ok; writes off (sbrtcwrite=1)
   or: StarbiteRTC: clock not used: clock not set (the year is not 2024-2099: the clock was never set); ...
STARBITE: sbstop=rtc: stage reached, resetting into TWRP
```
Pass: the register line appears with UPDATE's RUDR clear and no "RUDR did not clear" (answers open question 1).
A 2018 date is expected (the clock was never set) and is not a failure.

## M5b: one write

Boot-args: `... sbstop=rtc sbrtcwrite=1 sbrtcset=<host Unix time when the boot command is issued>`

I put the host's time into the command line right before `store-flash.sh`; the driver adds its uptime.

Expected:
```
StarbiteRTC: sbrtcset: wrote 2026-11-.. ..:..:.. UTC (...): ok (ok); reads back ... (ok)
```
Pass: `ok (ok)` and the read-back within 3 s of the written value (answers question 2). Fail
("read-back differs"): writes stay off; I look at WUDR/AUDR before anything else.

## M5c: correct date without the card clock

Boot-args: `... sbstop=rtc` (no write), at least a few minutes after M5b, with the card's SBTIME01 sector left stale
(I do **not** run the card-clock refresh for this boot).

Expected: `StarbiteRTC: clock <date> ..., ok` before any `STARBITE SD: calendar set ... from the card` line.

**Pass line:** the logged date is within 2 minutes of the host's UTC time at the moment of the boot command (the
host notes it), with no card read before it. That is M5's "correct date with no card clock".

Afterwards (not part of the test): integration may switch 0402's IORTC wait back on with a short timeout and turn
writes on by default (StarbiteRTC/INTEGRATION.md).
