# StarbiteACPM: first phone test (milestone M5, step a)

**Question:** does StarbiteACPM find the APM's channel table through the ADT and read the PMIC, with no write?

Nothing in this test writes a PMIC register, a regulator or a clock: `report()` only reads (PMICID, the five rails'
control registers, RTC CTRL / CAPSEL). It is the read path every later step depends on.

## Prerequisites (host, I do these)

1. M1 passed (StarbitePE + StarbiteARMIO publish the arm-io nubs from the board ADT).
2. The board ADT carries the SRAM for `acpm-ipc` (INTEGRATION.md step 1); `mkadt-board.py --check` passes.
3. Kernel: the M1 kernel + StarbiteACPM built in (INTEGRATION.md steps 2-4), **with the bsd/dev ACPM users
   switched off for this boot** (`startouch=0`, no GPU stage 2, no SD rails: hazard H1), i.e. the M1 command
   line, which already has none of them.
4. Dry run of the store flash, verified backup, as always (one phone).

## Catalog row and boot

A new hidden row `applem5acpm`, like `applem1` but with the loader's watchdog as the dead-man:

```
[[entry]]
key = "applem5acpm"
name = "Apple M5a test: ACPM read"
category = "lab"
method = "bare"
tag = "lab"
files = ["applem5.img"]
flags = ["hidden"]
wdt = 82
wdt_feed = "xnu"
```

Boot-args: `tools/cmdline/apple-m1.txt` with `sbstop=pe` replaced by `sbstop=acpm sbcrash=recovery`.

Run: `tools/store/store-flash.sh ... && tools/store/boot-safe.sh applem5acpm` (chained with `&&`).

## Dead-man (independent of the driver)

- `sbstop=acpm`: kernel 0423 never feeds the watchdog the loader armed (`wdt = 82`, `sbwdt=82` appended) and sets
  INFORM3 = recovery at boot (0422). Whatever XNU does (hang in the APM poll, panic, a stuck IOKit), the
  watchdog resets the phone into TWRP within 82 s of the hand-off.
- The driver's own stop: after its report line, `starlte_sbstop_now("acpm")` dumps the console tail into pmsg and
  resets into TWRP at once.
- Worst case for a hung ACPM poll: 3 sends x 600 ms per register, 8 registers = 14.4 s, well inside 82 s.

## Expected pmsg lines

```
StarbiteACPM: mailbox 0x14100000, SRAM 0x2039000 + 0x30000, initdata-base 0x7f00
StarbiteACPM: acpm: firmware "....", channel table at 0x..., N channels, M the AP's
StarbiteACPM: acpm: channel 2 (index i): owner . type ., queue L x E, polled, usable
StarbiteACPM: firmware "...", PMIC channel 2: PMICID=0x.. LDO35=0x.. LDO43=0x.. BUCK6=0x.. LDO31=0x.. LDO2=0x..
    RTC_CTRL=0x02 RTC_CAPSEL=0xf. (8/8 reads), 0 timeouts, k stale drained, INTMR1 0x.. INTSR1 0x..
STARBITE: sbstop=acpm: stage reached, resetting into TWRP
```

## Pass line

`StarbiteACPM: ... (8/8 reads), 0 timeouts` with LDO35 / LDO43 / BUCK6 / LDO31 / LDO2 equal to what 0302 / 0301 /
0345 logged on earlier boots (`STARLTE ACPM: PMICID ..., LDO35CTRL ...` in older pmsg files) and `RTC_CTRL=0x02`.

## If it fails

- No StarbiteACPM line at all: matching (nub name / compatible) or the SRAM property; look for "no APM SRAM".
- `NOT usable` channel 2 or timeouts: compare the channel table with 0302's own log line from an older boot.
- The phone comes back to TWRP either way; read `/sys/fs/pstore/pmsg-ramoops-0` (I pull it).
