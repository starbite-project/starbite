# StarbiteButtons: first phone test (milestone M8, input part)

The buttons are tested in the same boot as the touch screen: **`../StarbiteTouch/PHONE-TEST.md` T1**
(`sbstop=hid`, chained test loader with the 82 s dead-man that nothing feeds, `a5keys=0 startouch=0`).

What this driver adds to that test:

- Before booting: `sh starbite/apple-drivers/StarbiteButtons/test/run.sh` passes (67 checks).
- **Do not hold any key while the kernel starts** other than what the boot itself needs; a key held at start is
  ignored until released (the log says `held at start (ignored until released)`), which is correct but not a test.
- During the window the user presses **Volume Up** once (and, optionally, Bixby). Power is not pressed in T1: the
  boot must end with the 0423 stop, not a power event.

Expected lines:
```
StarbiteButtons: bank 0x60: con 0x...f.... dat ... pud ... (read only)
StarbiteButtons: bank 0x20: con ... dat ... pud ... (read only)
StarbiteButtons: registered: 4 keys, polled every 10 ms
StarbiteButtons: Volume Up down (usage 0xc:0xe9)
StarbiteButtons: Volume Up up (usage 0xc:0xe9)
StarbiteTouch: M8 ... ; key events 2
```

**Pass line:** `StarbiteButtons: Volume Up down (usage 0xc:0xe9)` followed by its `up` line, and `key events 2` (or
more) in StarbiteTouch's M8 result line. A `... is function 0x., not a key: ignored` line means the pin is muxed
to something else on this boot (no key events from it: correct, but report it).

Unattended variant: `sbkeyscript=9u` gives `Volume Up down, scripted` / `up, scripted` without a finger.
T2 (UI): on iOS 26 a Volume Up press shows the volume HUD on the photo; Bixby (Menu) goes home.
