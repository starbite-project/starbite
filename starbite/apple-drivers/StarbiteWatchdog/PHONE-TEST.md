# StarbiteWatchdog: phone tests (milestone M5: watchdog on, watchdogd checks in)

Observer mode only: the driver never writes the watchdog in these tests. The loader's watchdog and today's
herolte_ufs.c rules stay exactly as they are.

Prerequisites: M1 passed; kernel = M1 kernel + StarbiteWatchdog with SweetWatchdog.cpp left out (INTEGRATION.md);
dry run, verified backup.

## W1: the driver reads the watchdog and keeps out of a test boot

Hidden row `applem5wdt`, `wdt = 82`, `wdt_feed = "xnu"`. Boot-args: `tools/cmdline/apple-m1.txt` with `sbstop=pe`
replaced by `sbstop=wdtnever sbcrash=recovery` (a stage nothing reaches).

**Dead-man:** that is the test itself: 0423 never feeds, StarbiteWatchdog must not either; the loader's watchdog
resets the phone into TWRP ~82 s after the hand-off.

Expected:
```
StarbiteWatchdog: cluster 1 WTCON 0xff39 WTDAT ~65063 WTCNT ... (~82000 ms); sbstop= test boot (0423): XNU never
    feeds; ...; mode observer (never writes), period 0 ms, userspace timeout 0 s
```
**Pass line:** that line, then the phone in TWRP 80-90 s after the boot command, and the pmsg tail shows no feed
by anyone (no "feeder running" line from 0399 either).

## W2: watchdogd checks in (an iOS 26 boot that reaches launchd)

Not a self-stopping boot: it runs to launchd. Row with `wdt = 60`, `wdt_feed = "xnu"`, `sweetreboot=180`
(the 0421 dead-man: feeding stops at 240 s, or at 300 s with no frame), `sbcrash=recovery`, no `sbstop`.

**Dead-man:** 0421 in today's feeder (unchanged) + the 60 s watchdog + 0422's INFORM3 = recovery: the phone is
back in TWRP by 240 + 60 s at the latest, also if it hangs.

Expected: `StarbiteWatchdog: watchdogd selector 1 (...) -> ok` and the other selectors' first-use lines.
**Pass line:** watchdogd's selector lines appear and watchdogd is not in the crash list of the 0412 kill log.
That answers open question 1 before `sbwdtuser` is ever turned on.
