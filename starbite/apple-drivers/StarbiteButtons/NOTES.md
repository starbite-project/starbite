# StarbiteButtons: register facts and open questions

Sources: **[ours]** = `build/xnu-12377/bsd/dev/a5xelte_keys.c` (STARLTE branch, runs on the phone today) and
`iokit/Kernel/arm/hid/SweetHID.cpp` (`sweet_hid_key` usages), **[board]** = `tools/board/starlte.board`
(`pinctrl-alive`), **[refport]** = an earlier port's public `drivers/input/exynos_keys.c` (APSL-2.0 per its
`LICENSES.md`; read for facts and two ideas, no code taken; credit: an earlier port / Claudius for an earlier port). libsb not used.

| Key | Pin | HID usage (Consumer page 0x0C) | Source |
|---|---|---|---|
| Power (side) | ALIVE GPA2 pin 4 (bank CON 0x60, DAT 0x64) | 0x30 Power | [ours], [refport] |
| Volume up | GPA0 pin 3 (CON 0x20, DAT 0x24) | 0xE9 Volume Increment | [ours], [refport] |
| Volume down | GPA0 pin 4 | 0xEA Volume Decrement | [ours], [refport] |
| Bixby | GPA0 pin 6 | 0x40 Menu (stands in for Home, as today) | [ours], [refport] |

- ALIVE pin controller at 0x14050000 ([board] `pinctrl-alive`). Each bank: CON +0 (a nibble per pin: 0 input,
  0xF wake-up interrupt), DAT +4, PUD +8, DRV +0xC. DAT gives the level in either mode.
- Pins are pulled up on the board and grounded by the key: 0 = held. S-Boot already reads these keys, so the mux
  and pulls are set; the driver only reads.
- Debounce [ours]: sampled every 10 ms, a new state is taken after three equal samples (20 ms).
- From [refport] (ideas, reimplemented): a key held when the driver starts is ignored until released (someone still
  holding Volume Down from a Download-mode combo, or Power); a pin whose CON nibble is neither 0 nor 0xF is not a
  key (another driver owns it).
- `dispatchKeyboardEvent` (not a bare `IOHIDEvent`) so Apple's own chord handling in IOHIDEventService applies
  (power + volume NMI / stackshot chords, gated by Apple's own boot-args).

## Open questions

1. **Bixby as Home.** Today Bixby sends Consumer Menu (Home on iOS 26's iPhone 11 UI has no button; backboardd
   maps Menu to the home gesture). On macOS Menu does little. Map Bixby to another usage on macOS?
2. **Power on macOS.** Consumer Power is what Ventura got (power dialog / sleep). A MacBook's Touch ID power key is
   Keyboard page (0x07) usage 0x66 (`kHIDUsage_KeyboardPower`) from AppleEmbeddedKeyboard; the real IOHIDEventService skips
   chord handling for that usage. Kept Consumer Power until M9 shows what loginwindow wants.
3. **Interrupts.** Polling at 10 ms costs one MMIO read per bank (2) per tick. With StarbiteGPIO serving the ALIVE
   wake-up EINTs the keys could become interrupt driven and wake the phone from sleep.
