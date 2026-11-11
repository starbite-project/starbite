# StarbiteTouch + StarbiteButtons: first phone test (milestone M8, input part)

docs/APPLE-S9-DRIVERS.md Part 5, M8: "touch/buttons on real IOHIDFamily", pass line **touch reaches Settings /
the pointer moves**. Nothing here has run on the phone. House rules: one phone only (S9 #1), dry run and verified
backup first, boot by catalog key, `store-flash && boot` chained, one phone job at a time, the coordinator runs
every host step, the user only presses the phone's buttons and, in this test, touches the screen.

## Before any boot (host)

1. `sh starbite/apple-drivers/StarbiteTouch/test/run.sh` and `sh starbite/apple-drivers/StarbiteButtons/test/run.sh`
   pass (388 + 67 checks today).
2. Compile check of both `.cpp` and every `hw/*.c` (README command; see the report) is clean.
3. Integration done (both INTEGRATION.md files): IOHIDFamily built in, personalities, the `/arm-io/touch` and
   `/arm-io/keys` ADT nodes, `cmu-peric0` reg 0x4000. `mkadt-board.py --check` passes on the new ADT.
4. The kernel log of the build shows both classes linked: `nm` of the kernel has `__ZN13StarbiteTouch10gMetaClassE`
   and `__ZN15StarbiteButtons10gMetaClassE`.

## T1: the drivers under an independent dead-man (`sbstop=hid`)

**Question:** do the touch chip and the keys reach Apple's HID event service on the new drivers?

- **Image:** a chained test loader, as M1: `sh tools/apple/m1-chain.sh PAYLOAD.xnu build/apple-m8/m8-hid.img 82`.
  The menuchain stub arms cluster 1's watchdog before anything else and **nothing feeds it**: 82 s is the hardware
  maximum (16-bit count, prescaler 256, divider 128, 26 MHz; tools/menu/chain/pack.py). XNU with `sbstop=` never
  feeds the loader's watchdog (0423), sends panics and resets to TWRP, and resets into TWRP itself at the stage.
- **Store row:** as M1's: method `bare`, `wdt = 0`, `wdt_feed = "own"`, `no_launch = true`. Boot by its catalog key.
- **Boot-args:** today's `tools/cmdline/apple-m1.txt` (iOS identity ADT) with `sbstop=pe` replaced by
  `sbstop=hid sbhidwin=25 startouch=0 a5keys=0`. `startouch=0` and `a5keys=0` keep the old touch and key threads
  off the hardware (one owner per bus/pin). For the macOS reading of the same test add `sbtouchmode=1`.
- **Timing:** the window opens at the touch bring-up and lasts `sbhidwin` s; bring-up must come before ~50 s
  after the stub starts. M1's log gives the time from hand-off to IOKit; if the bring-up line is later than 50 s,
  lower `sbhidwin` (the stop must come before the watchdog, otherwise the result line is lost; the watchdog still
  brings the phone back to TWRP).
- **What the user does:** when the coordinator says "now" (about 20 s after pressing boot): tap the screen once,
  drag one finger, swipe two fingers up, then press **Volume Up** once. The screen shows no UI in this test.
- **Return:** the phone resets into TWRP by itself; the coordinator pulls `/sys/fs/pstore/pmsg-ramoops-0`.

Expected lines (pmsg; the two drivers start in either order):
```
StarbiteButtons: bank 0x60: con ... dat ... (read only)
StarbiteButtons: registered: 4 keys, polled every 10 ms
StarbiteTouch: iOS digitizer: panel 1440x2960, shown display 1367x2960
StarbiteTouch: registered (digitizer), bring-up on the work loop
StarbiteTouch: ACPM: after enable LDO35CTRL 0xec, LDO43CTRL 0xf0 (ok)
StarbiteTouch: USI03 in I2C mode, GPP1-4/5 muxed: SCL 1 SDA 1
StarbiteTouch: S6SY761: boot status 0x20 (application), device id .. .. .., panel range .... x ...., channels .. x ..
StarbiteTouch: sense on: ok, event stack cleared: ok; polling
StarbiteTouch: self-test PASS: 41 packets, 39 contacts; 39 digitizer frames, 7 from the edge, 14 touch changes
StarbiteTouch: sbstop=hid: touch the screen and press a volume key; the result and the stop come in 25 s
StarbiteTouch: live contact: finger 0 down at ...
StarbiteButtons: Volume Up down (usage 0xc:0xe9)
StarbiteButtons: Volume Up up (usage 0xc:0xe9)
StarbiteTouch: result: chip up, ...
StarbiteTouch: M8 PASS digitizer (touch reaches the HID system): chip boot 0x20 id ......, self-test pass, live contacts N -> frames M, ...; key events 2
STARBITE: sbstop=hid: stage reached, resetting into TWRP
```
With `sbtouchmode=1` the self-test line reads `self-test PASS: 41 packets, 39 contacts; ... button 1 x2, button 2 x1,
10 scrolls; taps 1, drags 1, secondary 1` and the result `M8 PASS pointer (the pointer moves): ... pointer events M`.

**Pass line (T1):** `StarbiteTouch: M8 PASS` with live contacts > 0, plus a `StarbiteButtons: Volume Up down`
line. Diagnosis when it fails:

| Line | Meaning |
|---|---|
| `SCL 0 SDA 0 (both low: no pull-up power...)` | supplies off: the ACPM line above says why |
| `does not answer: no ack` | chip off or not booted; `boot status` not 0x20 = the chip is in its boot loader |
| `reg is 0x1000 bytes, the driver needs 0x303c` | the cmu-peric0 board fix is missing |
| `self-test FAIL` | the gesture code differs from the host build (rebuild; compare with test/run.sh output) |
| no `StarbiteTouch:` line at all | personality / ADT node missing, or `IONameMatch` mismatch |
| live contacts 0 but the chip is up | INT pin (GPA1-0) not seen low: check `starbite,irq-pin` |

Unattended variant (user asleep): add `sbtouchscript=3t720:1480,6s300:600:900:1400 sbkeyscript=9u`. The result line
then says `M8 FAIL ... live contacts 0` by design; the pass for this variant is chip identified + self-test PASS +
`scripted step 2 of 2 done` + the scripted Volume Up lines.

## T2: touch reaches Settings / the pointer moves (attended only)

**Question:** does SpringBoard (iOS 26) / WindowServer (Sequoia, M9) act on these events?

This needs a full boot to the UI (70-85 s to the first frame on iOS 26), longer than any independent dead-man
the S9's watchdog can hold (82 s). So T2 can only run **with the user present**, using the existing timed-boot
dead-man (0421: `sweetreboot=S` stops feeding at S+60 s, `sbnoui=300`; `sbcrash=recovery`), which XNU feeds
(the T37 lesson: it cannot catch a hang XNU keeps feeding through; the user's Power + Volume Down hold is the
backstop). **Decision for the user:** accept that for T2, or keep T2 until StarbiteWatchdog gives a watchdog that
a hung kernel cannot feed.

- iOS 26: boot-args of the daily iOS 26 line + `startouch=0 a5keys=0 sbtouchscriptframe=1
  sbtouchscript=20s720:2955:720:1500,35t556:1406`: swipe up from the bottom edge (unlock), then a tap on Settings
  (panel coordinates of the Settings icon on the first home page, from sweetswipe=4's tour; re-read them on the
  last home-screen photo). The user then taps around Settings by hand.
- Sequoia (M9 kernel): `sbtouchscriptframe=1 sbtouchscript=30s300:600:1100:2000`: the pointer is steered along the
  drag; the user then taps a Dock icon.

**Pass line (T2):** iOS: the camera photo (Photo Booth) shows Settings open after the scripted tap, and pmsg has
`StarbiteTouch: digitizer: finger 9 down at 556,1406` followed by the user's own `digitizer: finger 0 down` lines.
macOS: the photo shows the pointer moved, and pmsg has `pointer: pointer hotspot measured` (or `steering by dead
reckoning`) and a growing `pointer events` count in the 60 s `stats` lines.
