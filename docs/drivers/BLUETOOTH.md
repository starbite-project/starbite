# Bluetooth (BCM4361, HCI over UART) in Starbite OS

Status 2026-11-12: written and host-tested. Not run on a phone yet.
Files: `starbite/sbos/kern/bt.c` + `bt.h` (power, firmware, state machine, probe), `bt_uart.c` (UART_BT, polled),
`bt_hci.c` (H4, events, .hcd, scan results, pairing groundwork; no hardware), `pcal6524.c` (the GPIO expander),
host test `kern/host/bt-test.c` + `bt-test.sh`.

## 1. What it is

The BCM4361 is one chip with two halves. Wi-Fi is on PCIe (`starbite/wifi`). Bluetooth is on a UART with HCI over H4.
Each half has its own power line: WLAN_REG_ON is SoC pin gpg0-5, BT_REG_ON is pin P0_7 of an I2C GPIO expander.

## 2. Wiring

Sources: the phone's own DT `device/dtb/starlte.dts` (DTS), an earlier port's notes in `bcm4361_bt.c` (facts only, APSL),
Samsung's GPL headers in `ref/starlte-kernel`.

| Signal | Where | Source |
|---|---|---|
| DT node | `bluetooth { compatible = "samsung,bcm43xx"; gpios = <0x4c 7 0  0x7e 0 0  0x81 3 0xf>; pinctrl-0 = <0x198 0x199> }` | DTS 13795 |
| BT_REG_ON | phandle 0x4c = `pcal6524@22` (compatible `"pcal6524,gpio-expander"`) on `hsi2c@10930000`; pin 7 = P0_7 | DTS 6676-6697 |
| Expander reset | `pcal6524,reset-gpio` = gpg3-6 (PERIC1 GPIO 0x108300C0), high = running | DTS 6688 |
| Expander registers | INPUT 0x00-0x02, OUTPUT 0x04-0x06, CONFIG 0x0C-0x0E (1 = input), pulls 0x4C/0x50 | `include/linux/i2c/pcal6524.h` |
| Other expander pins | P0_1/P0_4 DisplayPort mux, P0_6 fingerprint sleep, P1_0 wireless charging, P1_1/P1_3 NFC | notes `utility-hw-research.md` |
| Expander bus | USI10_I2C: pins gpp6-2 SCL / gpp6-3 SDA function 3; USI10 mode word 0x1081103C = 4 (I2C); CMU_PERIC1 gates +0x2050/+0x20A8/+0x210C/+0x2110 | DTS; an earlier port |
| BT_WAKE | phandle 0x7e = gpg2 (PERIC0 0x104300C0), pin 0, output (`bt-btwake`, pinctrl 0x199) | DTS 1731, 2670 |
| BT_HOST_WAKE | phandle 0x81 = gpa2 (ALIVE 0x14050060), pin 3, input (`bt-hostwake`, pinctrl 0x198) | DTS 192, 510 |
| UART | `uart@10840000` ("uart1", UART_BT), usi-serial-v2, 256-byte FIFO, status okay | DTS 9384 |
| UART pins | gpq0-0 RXD, -1 TXD, -2 RTS, -3 CTS (ALIVE +0xA0), function 2, pull-up (`uart1-bus`) | DTS 261 |
| UART clocks | CMU_PERIC1 gates +0x2034/+0x201C/+0x20C4/+0x20C8, Q-channel +0x3028, divider +0x1814, mux +0x0120 | an earlier port |
| 32 kHz sleep clock | from the S2MPS18; its RTCBUF register is 0x13 (PMIC map) | `s2mps18-private.h:55` |

Notes:
- gpq0-4/5 are the PMIC's SPEEDY bus. The driver only writes bits 15:0 of gpq0.
- gpg2-4 is the torch's pin. The driver only writes pin 0's bits of gpg2.
- S-Boot leaves UART_BT in its USI reset (an earlier port read USI_CON = 1 on this phone).
- The 32 kHz clock: Wi-Fi works in Starbite OS without any 32k write, so the chip's sleep clock is running when the
  OS starts. The driver reads RTCBUF and logs it; it never writes it.

## 3. Bring-up sequence (bt.c)

From Linux btbcm.c / hci_bcm.c and Samsung's bcm43xx.c (via an earlier port's notes; an earlier port's driver runs on this S9 and a paired
device stays connected).

1. Firmware: read the whole .hcd and check it before anything is powered. Order: VENDOR `firmware/bcm4361B2_semco.hcd`,
   `etc/bluetooth/...semco`, then `_murata`, then the card `/STARBITE/FW/bcm4361B2_<module>.hcd`. S9 #1's module is SEMCO
   (Wi-Fi read OTP ID a1 33). A bad file is refused (not a vendor command, truncated, no Launch_RAM, Launch_RAM early).
2. Look: pins, clocks, UART registers saved. A forced-off clock gate stops here (not opened unless
   `bt_allow_clock_gates(1)`).
3. Expander: bus up (mode word, pins, controller), out of reset if held, ports read. If BT_REG_ON is already an output
   high, stop: someone else powered it.
4. UART: pins to function 2 with pull-up, out of USI reset, interrupts masked, FIFOs on. The input clock is measured: 16
   bytes in the internal loopback at a known divisor, timed by the arch timer (a 200 MHz or a 26 MHz clock both work).
   115200 8N1, RTS/CTS on.
5. Power: BT_WAKE high; BT_REG_ON output low, 10 ms; high, 100 ms.
6. HCI_Reset, Read_Local_Version (the ROM).
7. Download_Minidriver (0xFC2E), 50 ms; each record sent and answered before the next; Launch_RAM (0xFC4E; an
   unanswered Launch_RAM is accepted); 250 ms.
8. HCI_Reset, Read_Local_Version (the patch).
9. Update_UART_Baud_Rate (0xFC18: two zero bytes, the rate le32), answered at the old rate; then the UART switches.
   Default 3 Mbaud (an earlier port used 4 Mbaud from 200 MHz; 0xFC45 was refused there and is not sent). From 26 MHz: 1 Mbaud.
   If the chip is silent at the new rate: chip off, on, patched again, kept at 115200.
10. Read_BD_ADDR. Only "address read" is logged, plus whether it is Broadcom's unprogrammed default (43:61:B2:..).
    The address stays in RAM for the UI. EFS is never read.
11. Set_Event_Mask (with LE Meta), Write_Inquiry_Mode 2 (RSSI / EIR), LE_Set_Random_Address (random static).
12. Ready. Scans on request: LE scan (active with the random address; passive if that failed), then a general inquiry.

Off: BT_REG_ON low then as found, BT_WAKE as found, UART registers as found (USI_CON last), pins as found, expander
reset / bus / controller / mode word as found, then everything read back.

## 4. Kernel API (bt.h)

`bt_hal_libsb(&h); bt_hal_set(&h);` once. `bt_on()`, then `bt_step()` every `bt_wait_ms()` until it returns 1.
`bt_scan(le_ms, inquiry_ms)` when `bt_state() == BT_READY`. `bt_ndev()` / `bt_dev_at(i)` give the devices heard,
strongest first (name, RSSI, LE or classic, class). `bt_off()` at any time. `bt_why()` is plain words for the UI:
"Bluetooth is off", "Bluetooth is starting", "Bluetooth is on", "The Bluetooth firmware is missing (...)",
"The Bluetooth chip does not answer (HCI_Reset after power-on)", and so on. Each `bt_step()` works at most ~20 ms.

## 5. The probe (drvprobe.h)

`int drv_bt_probe(int level, unsigned *wait_ms); void drv_bt_abort(void);` budget 55000 ms (level 1 uses 45 s itself).

- Level 0, read only: the BT pins and functions, WLAN_REG_ON's level, UART_BT's clocks and registers (when not gated
  off), the expander's bus pins / reset / controller (no I2C traffic: that would need writes), S2MPS18 RTCBUF, the
  firmware file read and checked. PASS: `drvprobe: bt: PASS read only; firmware <path> (<n> records, "<desc>")`.
- Level 1: the whole bring-up, LE scan 5 s, inquiry 5.12 s, the 3 strongest devices (RSSI, name), then off and
  everything back. PASS: `drvprobe: bt: PASS ROM <subver> -> patched <subver>/<rev> (<n> records), <baud> baud RTS/CTS,
  address <the chip's own|Broadcom's default>, LE <n> + classic <m> devices, strongest <rssi> dBm; everything back as
  found`. FAIL lines name the step in plain words.
- Level 2: as level 1 with LE 12 s + inquiry 10 s and every device listed.

Wi-Fi is not disturbed: the driver never writes WLAN_REG_ON, PCIe or any Wi-Fi clock; BT_REG_ON is a separate line;
the only shared code is the VENDOR reader (single-threaded). A write outside Bluetooth's own words is refused by an
allow-list in bt.c (and in pcal6524.c).

## 6. Host test

`sh starbite/sbos/kern/host/bt-test.sh` (`-v` prints the log): 65 checks. H4 framing, events, the .hcd checks, the
divisor, AD/EIR names, sorting, SSP Just Works answers; then the driver against a fake UART, expander and controller:
levels 0 / 1 / 2, every record checked in order, the baud change and its fallback, a silent chip, a corrupt .hcd
(refused, chip never powered, nothing written), firmware on the card only, no firmware, a refused record, an unanswered
Launch_RAM, the expander held in reset, BT already on, a closed clock gate, a stopped UART clock, a 26 MHz clock, an
abort mid-patch, the budget, and the kernel API. After each run: every register and expander bit as found, nothing
written outside the allow-list, the address never in the log.

## 7. Risks and what is left

- The UART_BT and USI10_I2C clock gates: Starbite OS does not open CMU gates. If either is forced off on the phone the
  probe FAILs with that reason; `bt_allow_clock_gates(1)` opens them (and restores) if the lead allows it.
- The expander bus has never been driven by Starbite OS. fled.c's notes record a first run on USI10 (the same USI, other half) that did not
  answer; the probe logs the controller state and INT_STAT on failure.
- If USI10's mode word was not I2C and the flash LED's bus starts using it during a Bluetooth run, the mode is left at
  I2C on close (logged).
- The download runs at 115200: about 10 s for the real patch (~440 records). Linux raises the rate first; that is a
  later speed-up.
- The module choice: SEMCO first. `bt_set_module()` lets Wi-Fi's OTP read decide.
- Not done: pairing and connections (the SSP Just Works answers are parsed and tested only), ACL, audio.
