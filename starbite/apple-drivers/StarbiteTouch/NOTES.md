# StarbiteTouch: register facts and open questions

Sources: **[ours]** = `build/xnu-12377/bsd/dev/starlte_touch.c` (0302, ran on the phone since Ventura/iOS 26),
**[sweethid]** = `build/xnu-12377/iokit/Kernel/arm/hid/SweetHID.cpp` (0323, 0071, 0373, 0378, 0394, 0397),
**[board]** = `tools/board/starlte.board`, **[refport]** = an earlier port's public `drivers/input/touch/s6sy761.c`
(APSL-2.0 per its `LICENSES.md` row for `*` files; read as a cross-check of facts, no code taken; credit:
an earlier port / Claudius for an earlier port), **[linux]** = mainline `s6sy761.c` and `i2c-exynos5.c` (GPL: facts only, as our 0302
did). libsb (GPL) was not used.

## Wiring

| What | Fact | Source |
|---|---|---|
| Chip | Samsung S6SY761 ("sec,sec_ts", firmware y761_star1), I2C 0x48 | [ours], [refport] |
| Bus | USI03 of PERIC0, HSI2C at 0x104B0000, 400 kHz, AUTO mode, reset before every transfer (DTB `samsung,reset-before-trans`) | [ours], [board] `hsi2c-touch` |
| Bus clock | CMU_PERIC0 0x10400000: gates +0x2018, +0x206C, +0x20DC, +0x20E0 (bit 20 manual, bit 21 on), Q-channel +0x3038 bits 1:0 | [ours], [refport] |
| USI mode | SYSREG_PERIC0 0x10411000 +0x1C = 4 (I2C) | [ours], [refport] |
| Pins | PERIC0 GPIO 0x10430000, GPP1 (CON +0x20, DAT +0x24, PUD +0x28, DRV +0x2C): pin 4 SCL, pin 5 SDA, function 2, no pull, lowest drive | [ours], [refport] |
| INT | ALIVE 0x14050000 GPA1 (CON +0x40, DAT +0x44) pin 0, active low (low while the event stack holds events) | [ours], [refport] |
| Supplies | tsp_io = S2MPS18 LDO35 (CTRL 0x6C, 1.8 V = 0x2C), tsp_avdd = LDO43 (CTRL 0x74, 3.0 V = 0x30), through ACPM IPC channel 2 (PMIC on SPEEDY, owned by APM firmware). Off after S-Boot on our phone: SCL/SDA/INT read 0 until they are on | [ours] |
| Panel | 1440 x 2960 pixels, about 64 x 132 mm | [ours], [sweethid] |

## HSI2C registers (offsets)

CTL 0x00 (bit 31 SW_RST, 7 TXCHON, 6 RXCHON, 3 MASTER), FIFO_CTL 0x04 (bits 1:0 FIFO on, RX trigger [10:4],
TX trigger [22:16]), TRAILING 0x08, INT_EN 0x20, INT_STAT 0x24 (W1C; 7 TRANSFER_DONE, 8 ABORT, 9 NO_DEV_ACK,
10 NO_DEV, 11 TIMEOUT), FIFO_STAT 0x30 (7 TX_FULL, 24 RX_EMPTY), TX_DATA 0x34, RX_DATA 0x38, CONF 0x40 (31 AUTO),
AUTO_CONF 0x44 (31 RUN, 17 STOP after transfer, 16 READ, [15:0] length), TIMEOUT 0x48 (31 enable), TRANS 0x50
(17 MASTER_BUSY), FS1/FS2/FS3 0x60/0x64/0x68 (9810 timing: FS1[23:16] start hold 0xF0, FS2[7:0] SCL-high mask
0xE0, FS3[23:16] divider 0x21 for 400 kHz from 200 MHz), ADDR 0x70 ([16:10] slave), USI_CON 0xC4 (USI reset,
active high: write 0). [ours]; bit names as [linux].

Note: [refport] programs the IP clock as 400 MHz with its own divider; ours (200 MHz assumption, divider 0x21) is
what has run on our phone, so it is kept. A logic-analyser check of SCL would settle it (open question 3).

## S6SY761 protocol

BOOT_STATUS 0x55 (1 byte, 0x20 = application), DEVICE_ID 0x52 (3), PANEL_INFO 0x23 (11: max x [0:1], max y
[2:3] big-endian, channels [8], [9]), SENSE_ON 0x10, CLEAR_EVENT_STACK 0x62, READ_ONE_EVENT 0x60 (8 bytes; byte 0
= 0: empty). Event: b0 [1:0] EID (0 coordinate), [5:2] TID = finger + 1, [7:6] state (1 press, 2 move,
3 release); x = b1 << 4 | b3 >> 4; y = b2 << 4 | b3 & 0xF; b4 major, b5 minor, b6 [5:0] z, b7 [5:0] events left.
The chip NACKs while it scans: 10 attempts 200 us apart. [ours], [linux]

Robustness kept from [ours]: at most 32 events per service, then release all + clear; INT low with an empty
stack for 200 polls (1 s) = release all + clear.

## The ported event logic (hw/sb_touch_gesture.c)

Unchanged behaviour of [sweethid]: 0397 direct touch (steer the pointer to the finger with relative events, one
correction per drawn frame, dead reckoning after 3 ticks without a redraw, hotspot measured by pushing the pointer
to 0,0, tap < 300 ms and < 40 px = click once there or after 20 ticks, hold 350 ms with < 20 px = drag),
0394 two fingers (24 px a line, natural; two-finger tap = button 2), 0373 absolute pointer (boot-arg), 0323/0071
digitizer (hand collection, finger children with identity = index + 2, display-integrated, edge 48 px ->
FromEdgeTip for the contact's life). One generalisation: 0397's scroll step was fixed at 24 px; it now follows
`sbtouchscroll` like 0373 did (default 24, same result).

## Open questions

1. **IOHIDFamily build.** 2238.120.5's public tree lacks `IOHIDEventTypes.h`, `IOHIDEventFieldDefs.h`,
   `IOHIDEventStructDefs.h`, `IOHIDEventMacroDefs.h` (checked with the GitHub tree API, 2026-11-11); 1915.140.3 has
   them. See INTEGRATION.md.
2. **Relative pointer acceleration.** Steering sends plain relative events (as 0397 did, proven on Ventura) and
   corrects in closed loop. `kHIDDispatchOptionPointerNoAcceleration` would remove the overshoot; untested.
3. **HSI2C clock.** 200 MHz + divider 0x21 (ours) vs an earlier port's 400 MHz branch reading. Both work on phones; SCL not
   measured.
4. **Interrupt.** The driver polls GPA1-0 every 5 ms (as today). With StarbiteGPIO serving EINTs, the node gains
   `interrupts`; the driver then switches to IOInterruptEventSource (+100 ms safety poll) by itself.
5. **Pointer position.** Steering reads SweetFramebuffer's `sweet_fb_cur_*`. StarbiteDisplay must export the
   cursor layer's drawn origin + a per-frame sequence (same symbols, or a property); without it, dead reckoning
   (works, less exact at screen edges).
6. **Supplies.** `starlte_pmic_*` from starlte_touch.c until StarbiteACPM offers a regulator call.
