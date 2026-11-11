# StarbiteWatchdog: facts, the contract, sources

Labels: **[V]** checked by us; **[S]** a third party says so; **[I]** our inference.

Sources: **HEALTH** = `build/xnu-12377/bsd/dev/starlte_health.h` and `herolte_ufs.c` (0399, 0421, 0422, 0423; our
code, phone-tested); **DOC** = `docs/STARBITE-HEALTH.md` 5.x (the vendor DT and mainline `s3c2410_wdt.c` facts);
**SWEET** = `SweetWatchdog.cpp` (0040, 0045; ios-sweet layer, MIT donor part per an earlier port's LICENSES.md).

## Hardware

| Fact | Value | Source |
|---|---|---|
| Block | cluster 1 (M3) watchdog 0x10060000, `samsung,exynos7-wdt`, SPI 0x1d1 (unused) | board file, DOC [V] |
| Registers | WTCON +0 (prescaler [15:8], ENABLE bit 5, divider [4:3] 16/32/64/128, INTEN 2, RSTEN 0), WTDAT +4, WTCNT +8 (16 bit), WTCLRINT +0xc | DOC [V] |
| Clock | OSCCLK 26 MHz | HEALTH, DOC [V] |
| Timeout | `count x (prescaler + 1) x divider / 26 MHz`; the loader's 60 s = WTCON 0xff39, WTDAT 47607 (59999 ms); maximum 82.6 s | HEALTH [V], host test cross-checks every value against HEALTH |
| PMU | AUTOMATIC_WDT_RESET_DISABLE 0x14060408, MASK_WDT_RESET_REQUEST 0x1406040c: bit 23 = cluster 1 (set = reset blocked) | DOC [V]. Not in this driver's nub: treated as "masked" |
| Feed | WTCNT = WTDAT | HEALTH [V] |

## The loader dead-man contract (kept)

1. `sbstop=` test boots (0423): XNU never feeds. **[V] HEALTH**
2. `sbwdt=0`: never fed. **[V]**
3. Never armed or re-programmed by XNU: WTCON / WTDAT only read. **[V]** (this driver: same)
4. No feeding once a panic starts. **[V]**
5. 0421: `sweetreboot=S` -> feeding stops at S + 60 s, and at `sbnoui` (300) s if no frame reached the panel. **[V]**
6. New: with `sbwdtuser=T`, once watchdogd has checked in, a gap over T s stops feeding for good.
7. Feed period: a third of min(sbwdt, hardware timeout), 1..27.5 s. **[V]**

`hw/sbwdt.c` implements 1-7 as one-way stops; `test/run.sh` checks every rule and compares the maths and the feed
decision with HEALTH over a grid of WTCON / WTDAT / PMU / sbwdt values.

## watchdogd interface

- `IOServiceMatching("IOWatchdog")`, user client type 1; watchdogd exits (and, being `_PanicOnCrash`, panics the
  system) if the service is missing; it also needs a service with `IOPMUPrimary = 1` [V SWEET comments, phone].
- Selectors: SWEET logged 1 (check-in), 2 and 9 (struct), 3/4 and 0/5 (userspace monitoring on/off) and accepted all
  with zeroed outputs [V SWEET]. Here selector 1 = check-in **[I]**; everything is accepted as before. The userspace
  rule (6) only arms after a check-in and only with `sbwdtuser=`, so a wrong guess about selector 1 can only leave
  the rule unarmed, never stop feeding wrongly... except if watchdogd checks in once and then uses another
  selector for later check-ins: then rule 6 would fire after T s. That is why `sbwdtuser` is off by default.

## Modes

- **observer** (default): never writes the watchdog. The existing feeder (herolte_ufs.c) keeps rules 1-5 and 7.
  With the integration hook `starlte_wdt_veto()` compiled in (`STARBITE_WDT_VETO_HOOK`), rule 6 stops that feeder.
- **owner** (`STARBITE_WDT_OWNER` build + `sbwdtown=1`): feeds itself under rules 1-7. Only once the herolte_ufs.c
  feeder is removed (two feeders would defeat rule 6 and 5).

## Hardware-safety notes

- Every failure mode ends in a watchdog reset (fail-safe): stopping the driver stops feeding.
- It never arms the watchdog, so a boot without the loader's watchdog stays without one (as today).
- Without `sbwdt=` the PMU words are unknown here, so nothing is fed: in owner mode a loader that armed the
  watchdog but dropped the token would see a reset after its timeout (herolte_ufs.c covers this today by reading
  the PMU; owner mode should get the PMU through StarbiteFirmware / StarbitePE first).

## Open questions

1. watchdogd's real selector meanings on iOS 26 / macOS 15 (log the first use of each; one boot).
2. The PMU mask words through a nub (StarbitePMGR or StarbiteFirmware) for owner mode.
