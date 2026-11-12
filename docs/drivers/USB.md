# USB device mode in Starbite OS

Starbite OS can show up on a computer as one USB serial port: a read-only console. On a Mac it is
`/dev/cu.usbmodem*` (macOS builds the name from the serial `SBOS`, so most likely `/dev/cu.usbmodemSBOS1`; any
terminal, e.g. `screen /dev/cu.usbmodemSBOS1 115200`). It streams the kernel log and answers
a few commands. It offers nothing else: no drive, no partition, no network (yet).

Status (2026-11-12): written and host-tested. **Never run on the phone.** Off by default (`SBOS_USB_AUTO 0` in
`kern/usb.h`) until the drvprobe's level 1 passes on the phone.

## Files

| File | What |
|---|---|
| `starbite/sbos/kern/usb.c`, `usb.h` | The glue: the cable, the D+/D- switch, the link's life, the console, the probe |
| `starbite/sbos/kern/usbacm.c`, `usbacm.h` | Starbite OS's descriptors and control requests (CDC-ACM only) |
| `starbite/utility/usb_dwc3.c`, `usb_phy.c`, `tb.c` | The Utility OS's drivers, linked as they are (`build.sh`) |
| `starbite/sbos/kern/host/usb-test.c`, `.sh` | Host test |
| `kern/main.c`, `kern/kern.h` | Two hooks: `usb_init()` at boot, `KT_USB` timer → `usb_timer()` |

## Wiring (with sources)

| Part | Where | Source |
|---|---|---|
| DWC3 USB 3.0 DRD, run as a USB 2.0 high-speed device | 0x10c00000 | vendor DT `usb@10c00000`; `utility/usb_dwc3.c` |
| USB PHY controller (USBCON) | 0x11100000 | vendor CAL `phy_exynos_usb_v3p1_enable`; `utility/usb_phy.c` |
| Clock gates (MANUAL bit 20, CG_VAL bit 21) | 0x1a242064 (CMU_TOP USB30DRD), 0x1100206c (FSYS0 REF), 0x11002008 (FSYS0 DP PHY) | `docs/USB-FASTBOOT.md` 4.2 |
| PMU PHY isolation (bit 0) | 0x1406072c (USB2), 0x14060704 (USB3/DP) | same |
| MAX77705 USB-C block | I2C 0x25 on HSI2C 0x14360000 (libsb `sb_usbc_read`, `sb_max_write`) | `utility/max77705_muic.c`; Samsung `include/linux/muic/max77705-muic.h` |
| VBUS / charger type | MAX77705 BC_STATUS 0x08: VBUSDET bit 7, ChgTyp bits 1:0 (1 SDP, 2 CDP, 3 DCP) | Samsung `max77705-muic.h` |
| D+/D- switch | MAX77705 MCU CONTROL1 via the opcode mailbox: 0x21 opcode, 0x22 data, 0x41 end marker, answer at 0x51 | Samsung `max77705_i2c_opcode_write`; `tools/linux/port/src/soc/s9p-max77705-muic.c` |
| DMA | 0xa7000000 + 1 MiB (libsb non-cacheable). Driver: first 16 KiB. Console: RX +0x8000 (512 B), TX +0x9000 (4 KiB) | `utility/usb_dwc3.h`; `libsb/mmu.c`; `wifi/sb/bcmw_sb.c` asserts the radio stays below |

USB identity: `1209:0001` (the project's pid.codes test PID, as the Utility OS, so a Mac that allowed it once may
allow it again), bcdDevice 3.00, product "Starbite OS (Galaxy S9)", serial `SBOS` (fixed, never the phone's own).
One configuration: IAD + CDC-ACM control interface (EP 0x81 interrupt, never sent) + data interface (EP 0x82 bulk IN,
0x02 bulk OUT), 75 bytes. `USBACM_PID` can be overridden at build time.

## How it runs

The kernel calls `usb_timer` from the main loop (owner `KT_USB`). Each call does one bounded turn:

1. **The cable** (every 500 ms with a cable, 1 s without): read BC_STATUS. A computer (SDP/CDP) starts the link.
   A cable whose type is still unknown after 2 s also starts it. A wall charger (DCP) does not. Survival's USB
   power out (the phone as the source) is never taken for a computer. Two low readings in a row = unplugged. The
   PHY has VBUS forced valid, so only the MAX77705 can see an unplug.
2. **The D+/D- switch.** The MAX77705's MCU owns it. A vendor kernel's shutdown leaves it open, and then the host
   sees nothing. So, as the loader, the Utility OS and Samsung's own MUIC driver do on every host attach: opcode
   CONTROL1_WRITE (0x06) with COM_USB (0x09), then the end marker. It goes through libsb's gate (tier 1, one
   authorisation per register, only 0x21 / 0x22 / 0x41). 50 ms later the value is read back (opcode 0x05, answer
   at 0x51/0x52). Written again 1.5, 3 and 6 s after the cable came in while no host has configured the phone.
3. **The start.** Snapshot of the 18 PHY / PMU / gate registers (only when nothing of ours is in them: "as the
   loader left them"), then `usb_phy_start` and `usbd_start("SBOS")`.
4. **Running**: up to 16 controller events per turn, the OUT transfer re-armed, up to 32 new log lines into the
   16 KiB TX ring, one IN transfer (at most 4 KiB, never a whole number of packets, so no zero-length packet).
5. **The teardown** (unplug, charger, Survival, `usb_enable(0)`, a failed start): `usbd_stop` (the loader's own
   stop: soft disconnect, transfers ended, events off, event buffer address cleared, core soft reset), then every
   PHY / PMU / gate register that differs from the snapshot is written back. Order: USBCON first (UTMI suspend,
   COMMONONN, SIDDQ, resets), then the PMU, then the gates. The core is always stopped before its clock is gated.
6. **Retries.** A failed start: down, as found, again after 5, 10, 20, 40, 60 s. A link no host configures in 10 s
   (a Mac waiting for "Allow accessory"): restarted, then after 20, 40, 60 s.

**Poll rate and cost.** 2 ms while a host enumerates or output is queued, 10 ms when configured and idle, 500 ms /
1 s for the cable alone. A turn is usually well under 0.1 ms (register reads, a few DEPCMDs). The start turn is
about 12 ms (PHY ~2 ms, core soft reset + 10 ms settle); at most ~0.32 s if the soft reset or run/stop never
completes (`usb_dwc3.c`'s own limits). The stop is about 2 ms (at most ~0.2 s). The main loop's own slow turns
(a heavy frame) add latency; the host's control-transfer timeouts (50 ms for SET_ADDRESS's status, 500 ms for
data) are far above the 2-10 ms poll, but a 200 ms loop turn during enumeration could make the host retry.

## The console

Open the port: a banner, then the kernel log as it is written. Commands (none writes anything or changes a setting):

| Command | What |
|---|---|
| `help` | the list |
| `ver` | version, time since the start, the USB identity |
| `bat` | battery %, mV, mA (and average), temperature, charger (the kernel's last sample, no new I2C read) |
| `log [N]` | the log ring (512 lines) from its start, or its last N lines |
| `follow on` / `follow off` | new log lines as they come (on when the port opens) |
| `usb` | this link: state, speed, address, resets, setups, stalls, starts, restarts, failures, the switch, bytes |

Line editing: backspace, ^C (drops the line and a running replay), CR, LF or CRLF. Anything else is "unknown
command". If the TX ring is full the log waits in the log ring; if that wraps first, a "[log lines were lost]" line
says so. Closing the port (DTR off) cancels the IN in flight and stops output.

## Kernel API (usb.h)

- `usb_init()`, `usb_timer(id)`: main.c only.
- `usb_enable(int on)`: for a future Settings switch. Off = link down, PHY as found, timer cancelled.
- `usb_status(struct usb_status *)`: state, VBUS, charger type, configured, terminal open, speed, address, switch,
  counters, last failure.
- `usb_why()`: one line for the UI: "USB off", "Not connected", "Charger (no data)", "Cable in: checking",
  "Waiting for the computer", "Connected to a computer", "Computer: console open", "USB did not start (trying again)".

## The probe (drvprobe.h)

`int drv_usb_probe(int level, unsigned *wait_ms)`, `void drv_usb_abort(void)`, budget `DRV_USB_BUDGET_MS` 15000.

- **Level 0** (one call, reads only): MAX77705 BC_STATUS / USBC_STATUS1 / CC_STATUS0; the 18 PHY / PMU / gate
  registers; the DWC3's GSNPSID / GCTL / DCTL / DSTS / DCFG / GEVNTADRLO / GEVNTSIZ / GUSB2PHYCFG, **only if the
  TOP and FSYS0 REF gates are not forced off** (MANUAL set, CG_VAL clear): a read of an unclocked block could hang
  the bus. All reads go through `sb_probe32`. PASS: the MAX77705 answered, the registers read, GSNPSID is a DWC3
  (0x5533xxxx or 0x3331xxxx) or the DWC3 was not read because it is gated.
- **Level 1** (VBUS from a computer only, else PASS "skipped"): route the switch, start PHY + core as the console,
  run for up to 10 s (or 2 s after the host configured it), dump `usbd_diag` / `usb_phy_diag`, stop the core, read
  DCTL / DSTS / GEVNTADRLO while its clock still runs, put the PHY registers back, and compare all 18 with the
  snapshot. PASS = enumerated (address set, configured), core stopped clean, every register as found.
- **Level 2**: nothing (PASS at once).
- **Abort**: stops the core (if it runs) and writes the registers back. Safe at any point, safe twice.

Pass / fail lines (all start `drvprobe: usb: `):

- `PASS DWC3 id 0x5533....  stopped, PHY powered, VBUS in from a computer (SDP)` / `PASS DWC3 clock gated (not read), ...`
- `FAIL the MAX77705 USB-C block did not answer on I2C ...`, `FAIL no DWC3 at 0x10c00000 (GSNPSID ...)`,
  `FAIL a DWC3 register read faulted`, `FAIL the USB PHY / PMU / clock-gate registers did not read`
- `PASS enumerated: address N, configured, high speed; stopped and put back as found`
- `PASS level 1 skipped: no USB cable from a computer (VBUS low)` / `... a wall charger (DCP), not a computer`
- `FAIL the host read N descriptors, set an address, never configured it (new accessory?)` (the Mac's
  "Allow accessories" prompt), `FAIL up for 10 s but the host sent nothing (no SETUP: ...)`, `FAIL <the driver's
  why>` (e.g. `DWC3 soft reset stuck (no PHY clock?)`), `FAIL the cable was pulled during the probe`,
  `FAIL the DWC3 did not stop cleanly ...`, `FAIL the PHY / PMU / clock-gate registers are not back as found`

### How level 1 leaves USB for the main session

The main session reaches the phone over USB again afterwards (TWRP's adb after the probe image restarts). This is
guaranteed four ways:

1. The core is stopped with `usbd_stop`, the same function the loader runs (`fbbg_off`) before it starts any OS:
   the DWC3 ends in exactly the state the loader hands over.
2. Every PHY / PMU / clock-gate register `usb_phy_start` changed is written back to the value read before the
   start, after the core stopped; the probe then reads all 18 again and FAILs if one differs.
3. The probe image restarts the SoC into recovery afterwards: DWC3, USBCON, CMU and PMU are reset anyway, and
   TWRP's kernel runs its own full USB init.
4. The only state that survives a restart is the MAX77705's CONTROL1 = COM_USB. That is the value Samsung's MUIC
   driver (TWRP, Android) itself sets with a host attached, and the loader's fastboot sets on every boot it serves.

So level 1 is on by default in the probe; no opt-in `#define` is needed.

## Risks

- **Never run on the phone.** The DWC3 / PHY / MAX77705 code did enumerate on the Mac from the loader and the
  Utility OS (2026-11-07). New here: the glue, the descriptors, the gated MAX77705 writes through libsb's `sb_max_write`
  (single-byte writes into the mailbox instead of one block write: the MCU acts on the end marker, so this should
  be the same; the read-back says whether it took), and the PHY write-back.
- **The Mac's accessory approval.** `1209:0001` with a new configuration may be held until the user allows it.
  Then level 1 FAILs with "never configured it (new accessory?)" and the service restarts the link every 10-60 s.
- **Level 0 reads USBCON / PMU / CMU registers with the USB clocks possibly gated.** The Utility OS's own
  `usb_phy_start` reads the same registers before opening the gates and ran on the phone, so these are believed
  safe. The DWC3 itself is not read while gated.
- **Restoring S-Boot's PHY state** writes USBCON back (SIDDQ on, resets) with the core already stopped. If a
  restore value ever upsets the PHY, the next start runs the full vendor sequence again anyway.
- **Polling latency.** A main-loop turn of 200 ms during enumeration could make the host retry an enumeration.
- `SBOS_USB_AUTO 0` until level 1 passes. Then the lead flips it to 1 (or adds a Settings switch through
  `usb_enable`).

## Not built: CDC-ECM / NCM networking (plan)

Goal: the Mac (and Linux apps on the phone) reach the phone over IP on the same cable.

1. Descriptors: a second function after the ACM one: IAD + CDC-NCM control interface (class 02/0d, EP 0x83
   interrupt IN for NETWORK_CONNECTION / SPEED_CHANGE), data interface with alt 0 (no endpoints) and alt 1
   (EP 0x84 bulk IN, 0x04 bulk OUT). NCM rather than ECM: macOS has a built-in NCM driver (since 10.15) and Linux
   and Windows 10+ too; ECM is not on Windows. A locally administered MAC (fixed, e.g. 02:53:42:4f:53:01, never the
   phone's Wi-Fi MAC) in string 5. `usb_dwc3.c`'s `EPN` (10) covers EP 0x84 (physical 9); the alternate setting
   needs `usb_ch9_setup`'s SET_INTERFACE to accept alt 1 on the data interface and the driver to (re)configure those
   two endpoints (today SET_INTERFACE only acknowledges alt 0: a small additive change in `usb_dwc3.c`'s
   `configure`, or doing it from the glue with a new `usbd_ep_enable` call).
2. Class requests: GET_NTB_PARAMETERS (wNtbInMaxSize 16 KiB, divisor 4), SET/GET_NTB_INPUT_SIZE,
   SET_ETHERNET_PACKET_FILTER (ack), GET_NTB_FORMAT (NTB16 only).
3. Data path: NTB16 parsing on OUT (NTH16 + NDP16 → Ethernet frames), NTB16 building on IN (one or several frames
   per transfer), 16 KiB DMA buffers each way in the USB window (+0x10000 / +0x14000).
4. The network stack: `starbite/wifi/net.c` + `tcp.c` already give the kernel ARP / IPv4 / TCP over the Wi-Fi
   link. A second link (the USB one) behind the same `net` interface: link-local 169.254.83.1 on the phone, a
   tiny DHCP server (one lease, 169.254.83.2) so the Mac configures itself, no routing between USB and Wi-Fi.
5. Services: read-only first (an HTTP page with the log and the status), same rule as the console: nothing that
   writes storage or changes settings without the phone's own confirmation.
6. Host test: the NTB16 codec and the class requests against a fake host, as `usb-test.c`.

Estimated size: ~600 lines (descriptors 80, NCM codec 250, glue 150, DHCP 120) plus the test.
