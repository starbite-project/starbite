# StarbiteUSB phone test: M11 "ping over NCM"

Status: not run. Nothing here has touched the phone. House rules apply (`docs/APPLE-S9-DRIVERS.md` Part 5): one
phone, boot by catalog key, `store-flash && boot` chained, I run every host step, you only press buttons.

Two tests. T1 needs only the kernel (no IONetworkingFamily, no user space) and can run as soon as the M1/M2 kernel
with ADT v2 boots. T2 is the real M11 line and needs M9.

## The one question of T1

Does the Mac get an answer to `ping6` from the phone over our own DWC3 + NCM code?

## Before the first run (host only)

1. `sh starbite/apple-drivers/StarbiteUSB/test/run.sh` passes.
2. The kernel has the driver (INTEGRATION.md), the ADT has the usb-phy `reg` entries 1 to 5, and the image's log
   shows `StarbiteUSB:` strings (`strings kernel | grep "M11 PASS"`).
3. The row's command line is checked on the Mac before flashing (see "Boot-args").
4. **You are at the Mac for the first run.** See "The Mac side".

## Boot-args

Added to the milestone kernel's usual command line:

```
sbusb=1 sbusb_echo=1 sbstop=usbncm
```

- `sbusb=1`: the driver is off without it.
- `sbusb_echo=1`: the driver itself answers ping (IPv6 on `fe80::53:42ff:feXX:XXXX`, the link-local address of its
  MAC; also IPv4 169.254.83.66). No IP stack on the phone is involved.
- `sbstop=usbncm`: this is a self-stopping test boot (kernel 0423 or later). Not for the first run: `sbusb_ring`,
  `sbusb_irq` (both untested models; see "Later runs").

## Dead-man (independent of XNU and of the Mac)

- The catalog row has `wdt = 82` (the most the loader allows), `wdt_feed = "xnu"`, and is **not** `no_launch`.
  The loader arms the hardware watchdog right before the hand-off and appends `sbwdt=82`.
- With `sbstop=` set, XNU **never feeds** that watchdog (0423), and panics, the watchdog reset and the stop itself
  all go to TWRP (`sbcrash=recovery` behaviour, 0422). So whatever happens (driver hang, kernel hang, the Mac never
  accepting the device, nobody pinging) the phone is back in TWRP at most 82 s after the hand-off.
- Check in pmsg afterwards that the line `STARBITE HEALTH: sbstop=usbncm: test boot (0423): the watchdog is never
  fed` does **not** end in `WARNING: the loader armed no watchdog`. If it does, the row was wrong: fix it before
  any further run.
- Boot with `tools/store/boot-safe.sh KEY` (it refuses a row without `wdt`).
- The driver never waits for the host. `start()` is bounded (about 0.35 s of register waits at worst, 1 s if the
  usb-phy nub is missing) and returns false on failure; the boot goes on without USB.

Time budget inside the 82 s: kernel to IOKit matching (known from M1/M2 logs; if that alone is over about 40 s,
this test does not fit and needs a shorter boot first), enumeration about 2 s, interface up and link-local on the
Mac a few seconds, ping 2 s.

## The Mac side

A new USB identity can stall on the Mac waiting for "Allow accessory to connect?" (memory note
`new-usb-identity-needs-approval`). This device reuses the loader's VID:PID 18d1:4ee0, which the Mac has accepted
before, but it is a different device behind it (bcdDevice 0x0200, other product string, an NCM function), and what
macOS keys its approval on is not known. So:

- **First run: with you at the Mac.** If a prompt appears, allow it. If macOS shows "New network interface
  detected", accept it. Tell me what appeared; I record it here.
- I start, right after the boot command:
  `sh starbite/apple-drivers/StarbiteUSB/test/mac-ping.sh 70`
  It waits for the USB device, then for the `en` interface whose MAC starts `02:53:42`, then for its link-local
  address, then runs `ping6 -c 5` to the phone. No sudo, nothing changed on the Mac. It always returns within
  about 85 s with a distinct exit code (2 no USB device, 3 device but no interface = approval pending or no NCM
  driver, 4 interface not active, 5 no answer), so my watcher is never held by the Mac.
- **Unattended runs** are allowed only after one run where the Mac made the interface with no prompt, and only
  with `sbstop=` and the `wdt` row as above. Even if the Mac stalls, the phone still returns on its own: the Mac
  cannot strand it.
- Never boot this without `sbstop=` unattended: without it the kernel feeds the watchdog and a healthy phone with
  an unreachable USB link sits there (the Utility OS case of 2026-11-07).

## Expected log lines (pmsg, read in TWRP after the return)

```
StarbiteUSB: start: transfers single, events by timer, echo on, serial <serial>, DMA 0x...
StarbiteUSB: device 02:53:42:xx:xx:xx = fe80::53:42ff:fexx:xxxx, host MAC ends xx
StarbiteUSB: PHY up (line J), DWC3 0x5533xxxx running as 18d1:4ee0, waiting for a host
StarbiteUSB: host connected, high speed (resets N)
StarbiteUSB: configuration 1 set by the host (setups N, stalls N)
StarbiteUSB: NCM data interface open: link up (high speed)
StarbiteUSB: echo: neighbour solicitation answered
StarbiteUSB: M11 PASS: echo reply 1 sent over IPv6 (ping over NCM)
StarbiteUSB: M11 PASS: echo reply 3 sent over IPv6 (ping over NCM)
StarbiteUSB: sbstop=usbncm: rx N frames in N NTBs (0 bad), tx N frames in N NTBs; stopping
STARBITE: sbstop=usbncm: stage reached, resetting into TWRP
```

## Pass line

Both: `StarbiteUSB: M11 PASS: echo reply 3 sent over IPv6 (ping over NCM)` in pmsg, **and** `mac-ping.sh` printing
`PASS: the phone answers ping over NCM (enX)` with 0 % loss.

## If it does not pass: what the log says

| Seen | Meaning | Next |
|---|---|---|
| no `StarbiteUSB:` line at all | `sbusb=1` missing, the personality did not match the usb-drd nub, or the kernel never reached IOKit matching | check the command line and `ioreg`-style dump of arm-io in the milestone log |
| `the usb-phy nub lacks reg entries (block mask 0x7c)` | ADT without the PMU / gate entries; the PHY is only as the loader left it | add them (INTEGRATION.md) |
| `start failed at stage -1` | USBCON did not keep the settings: PHY unpowered or wrong block | compare UTMI / HSP_TEST in the line with the loader's `oem usb` |
| `start failed at stage -3` (soft reset stuck) | no PHY clock: gates or CMU mux (the loader's known failure mode) | `docs/USB-FASTBOOT.md` "Clocks" |
| `no host yet: line SE0 ... resets 0` repeatedly | D+/D- do not reach the connector: the MAX77705 switch is not on the AP (NOTES.md open question 1), or no cable | boot once through the loader's menu (its background fastboot closes the switch), or wait for the MUIC driver |
| `no host yet: line J ... resets 0` | pulled up, but the host does not see it | cable / hub; try the port the loader's fastboot uses |
| resets counted, `setups 0`, then `link restarted from the PHY` | the reset storm the loader's notes describe | already retried with back-off; read the counters |
| setups counted, never `configuration 1` | the Mac read descriptors and stopped: accessory approval, or it refuses a descriptor | `mac-ping.sh` exit 3 shows `UsbEnumerationState`; you look at the Mac's screen |
| `configuration 1` but never `data interface open` | macOS's NCM driver did not bind (NOTES.md section 6) | capture `ioreg -p IOUSB -l`; fall back to ECM descriptors |
| link up, no `neighbour solicitation answered` | nothing arrives on bulk OUT, or NTBs are rejected (`rx ... (N bad)` in the stop line) | the stop line's counters decide |
| solicitation answered, no echo reply on the Mac | bulk IN or the interrupt endpoint | the stop line's counters and `cmd failures` in the last `no host yet` line (the single-TRB model is already the default) |

One run answers one question. Whatever the outcome, the phone is in TWRP afterwards; I pull pmsg, explain, and plan
the next run.

## Later runs (each its own boot, same dead-man)

1. `sbusb_ring=1`: the ring transfer model. Same pass line.
2. `sbusb_irq=1` (after the board file carries SPI 244 for usb-drd): the start line says `events by interrupt`.
3. A longer `ping6 -c 200 -i 0.01 -s 1400` within the budget, to load the TX aggregation and RX path; the stop
   line must show `0 bad`.

## T2: M11 proper (after M9, with IONetworkingFamily)

Sequoia on the phone with StarbiteUSBNCM attached: `sbusb=1`, no `sbusb_echo`, no `sbstop`. The phone's `en`
interface gets its own link-local address; `mac-ping.sh` does not apply (its address is the echo responder's).
Instead: `ping6 -c 5 ff02::1%enX` on the Mac lists the phone, then `ssh` to it. Dead-man for T2: this is a full OS
boot, so it follows M9's rules (a row with `wdt` and a proven independent reset, `sweetreboot=` for timed runs);
with the user present for the first one. Pass: 0 % loss, and
`StarbiteUSBNCM: Ethernet interface attached` + `NCM data interface open: link up` in the log.
