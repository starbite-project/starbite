# StarbiteUSB notes: register facts, sources, what is verified

Labels as in `docs/APPLE-S9-DRIVERS.md`: **[V]** checked by us in a repo file, a doc or a phone log; **[S]** a third
party says so; **[I]** inferred, not yet run on the phone. "Loader" = `shim/starlte/usb_dwc3.c`, `usb_phy.c`,
`usb_desc.*`, `fastboot_dev.c`; `docs/USB-FASTBOOT.md` ("First hardware run") records it enumerating on this Mac at 480 Mb/s.

Nothing in this folder has run on the phone. Host tests and compile checks only.

## 0. Licence finding (please read)

The task brief called the loader's USB code "ours (not GPL)". The files say otherwise: `shim/starlte/usb_dwc3.c`,
`usb_dwc3.h`, `usb_phy.c`, `usb_phy.h`, `usb_desc.c`, `usb_desc.h`, `fastboot_dev.c` and `fbbg.h` all start with
`SPDX-License-Identifier: GPL-2.0-only`, and their heads say they follow Linux's dwc3 and Samsung's CAL. an earlier port's
`LICENSES.md` notes the same about the loader ("several follow the Linux and Samsung drivers for the DWC3").
So I treated them like `starbite/libsb`: **register documentation only**. No code was copied. What is taken is
facts: register offsets, bit positions, values, and the order of steps the hardware needs. The code here is written
new (the PHY is a table of steps, endpoint 0 and the rings are new functions and structures). The control-transfer
state machine necessarily has the same states as any DWC3 device driver, because the controller dictates them.
If the author wants a cleaner position, he can relicense his own loader files (he is the copyright holder of his
parts), or have this reviewed. an earlier port's `drivers/usb/exynos_usb.c` (APSL-2.0 by its `LICENSES.md` row "04xx /
*") documents the same PHY steps a to l and is credited in `hw/sbusb_phy.c`; no code was taken from it either.
Linux was not consulted for this driver.

## 1. Addresses and interrupts

| Item | Value | Source |
|---|---|---|
| DWC3 | 0x10c00000 + 0x10000 (globals at +0xc100, device at +0xc700, endpoints at +0xc800) | [V] `tools/board/starlte.board` usb-drd; loader |
| DWC3 interrupt | SPI 244 (0xf4), INTID 276 | [V] `device/dtb/starlte.dts` `usb@10C00000/dwc3` `interrupts = <0 0xf4 0>`. **The board file has `spi = []` for usb-drd**: see INTEGRATION.md |
| USBCON (PHY glue) | 0x11100000 + 0x200 | [V] board usb-phy; loader |
| PMU USB 2.0 PHY isolation | 0x1406072c, bit 0 = enabled | [V] loader `usb_phy.c` |
| PMU USB3/DP PHY isolation | 0x14060704, bit 0 = enabled | [V] loader (an earlier port's glue leaves this one alone [S]) |
| Clock gates | 0x1a242064 (CMU_TOP USB30DRD), 0x1100206c (FSYS0 REF_CLK), 0x11002008 (FSYS0 DPPHY): set bits 20, 21 | [V] loader |
| DWC3 ID | GSNPSID 0x5533xxxx (or 0x3331xxxx) | [V] loader checks this; the exact revision on the S9 is in `oem usb` output, not recorded in the docs I read |

The five PMU / clock-gate registers are **not** in either nub's `reg` today. The driver takes them from the usb-phy
nub as `reg` entries 1 to 5 and skips (and logs) any that are missing. It never uses a hard-coded address.

## 2. PHY sequence (`hw/sbusb_phy.c`)

All steps [V] from the loader (which enumerates on the Mac) and, for the USBCON part, [S] an earlier port's glue: gates,
PMU, Q-channel (DIS_QACT bits, 500 us, FORCE_QACT), link reset pulse, PHY reset high, SIDDQ off, UTMI overrides
off, EN_UTMISUSPEND + COMMONONN, 100 us, PHY reset low, VBUS forced valid (no VBUS pad on this board) with the bus
filter bypassed, OCD port select, ReWA parked, PIPE3 forced idle + PMA low power, HSP_TUNE stock device values.
Host test `t_ring.c start_states` checks every final register value against these facts.

Not done, as in the loader: CMU muxes and dividers are not written (rule against touching them; the loader relies on
S-Boot leaving them on a running source [V] `docs/USB-FASTBOOT.md` "Clocks"), LDO modes are not changed.

## 3. DWC3 core start (`hw/sbusb_dwc3.c sbusb_dwc3_start`)

Every step [V] loader, same order: CSFTRST (needs the PHY clock; up to 200 ms) then 10 ms; GUSB2PHYCFG clear SUSPHY,
ENBLSLPM, FREECLK_EXISTS, PHYSOFTRST; GUSB3PIPECTL clear SUSPHY; GCTL device mode + SOFITPSYNC, scale-down off,
clock gating off only on 2.10a..2.50a; GSBUSCFG0 INCR + INCR16 (+ REQINFO from 2.20a), GSBUSCFG1 burst limit 3; GUCTL
REFCLKPER 0x14 + HSTINAUTORETRY; GFLADJ 0x20; DCFG high speed; one 4 KiB event buffer; DEVTEN; ep0 STARTCFG(0),
SETEPCONFIG 512 both directions, SETXFERRES; DALEPENA 3; SETUP TRB; RUN_STOP; wait for DEVCTRLHLT to clear.

Differences from the loader, all **[I]**:
- DEVTEN also enables the link-state event (bit 3) so a USB suspend drops the network link. An unknown event is
  ignored, so the risk is small, but the event encoding (type 3, link state in bits 19:16, U3 = 3) is from the
  DWC3 documentation, not from a phone log.
- A controller found running (DCTL RUN_STOP set) is halted and drained before the PHY is reset. an earlier port's driver does
  the same [S]. After the loader's `fbbg_off` it is never running [V].
- `sbusb_irq=1` leaves GEVNTSIZ's interrupt mask clear. Never run. Default is the loader's polling.

## 4. Endpoint 0

[V] loader for the sequencing: SETUP TRB on physical endpoint 0 (ISP, LST, IOC); data stage TRB started at once
(IN on 1, OUT on 0, OUT length rounded up to 64); status TRB only on XferNotReady(status), STATUS2 without and
STATUS3 with a data stage; SET_ADDRESS into DCFG at setup time; Connect Done sets ep0 to 64 bytes with MODIFY;
SETUP_PENDING TRB status re-arms; a control transfer stuck for 1 s is stalled and re-armed; a zero-length packet
when the answer is shorter than wLength and a multiple of 64.

New here and **[I]**: the NCM requests (standard USB, checked against the spec in `t_ep0.c`), SET_INTERFACE with
two alternate settings (the loader has none), SET_FEATURE(ENDPOINT_HALT), and stalling the status stage when
SetNtbInputSize carries an out-of-range value.

## 5. Data endpoints and TRB rings

Endpoint assignment: 0x81 interrupt IN (physical 3, TX FIFO 1), 0x82 bulk IN (physical 5, TX FIFO 2), 0x02 bulk OUT
(physical 4). The loader uses exactly these three for its ACM function [V], with FIFO number = endpoint number.
SETEPCONFIG / SETXFERRES / DALEPENA at SET_CONFIGURATION with STARTCFG parameter 2 [V].

Two transfer models, same ring bookkeeping (`struct sbusb_ring`, 7 TRBs + 1 link slot):
- **single** (default): each TRB has LST; one is started (Start Transfer), XferComplete, then the next. This is the
  loader's model **[V]** for bulk; used here also for the interrupt endpoint, on which the loader never sent **[I]**.
- **ring** (`sbusb_ring=1`): no LST, CSP on OUT, a link TRB (type 8, HWO) back to TRB 0, Start Transfer once, Update
  Transfer (command 7) after each new TRB, XferInProgress events (DEPCFG parameter 1 bit 9). **[I]** entirely: this
  is the databook model as I know it; none of UPDATEXFER, the link TRB or XferInProgress has run on this phone. It
  is there because the task asked for rings and it halves the command traffic; the first phone test uses single.

CLEAR_FEATURE(ENDPOINT_HALT) ends the transfer, issues CLEARSTALL (which resets the data toggle) and re-arms [V] the
loader does the same for its one TRB.

## 6. NCM

Descriptors and requests are from the specifications (USB 2.0 ch. 9, CDC 1.2, ECM 1.2 5.4, NCM 1.0 5.2, 6.2, 6.3)
and are checked byte by byte in `t_desc.c`, `t_ncm.c`, `t_ep0.c`. Choices:
- 16-bit NTBs only; dwNtbInMaxSize = dwNtbOutMaxSize = 16384; divisor 4, remainder 0, alignment 4.
- bmNetworkCapabilities = 0x01 (SetEthernetPacketFilter answered). No CRC, no max-datagram-size, no net-address,
  no 8-byte SetNtbInputSize.
- An NTB below the maximum size never ends on a packet boundary (one zero byte is added), so no zero-length packet
  is ever needed on bulk IN.
- Notifications on the interrupt endpoint after SET_INTERFACE(alt 1): ConnectionSpeedChange (16 bytes) then
  NetworkConnection(1) (8 bytes).
- **[I]** That macOS's host-side NCM driver binds to this descriptor set. Apple ships a host NCM driver (iPhones
  use NCM), and devices of this shape are reported to work [S], but it is not tested with this device. If it does
  not bind: the known-good fallback is CDC-ECM, which this Mac already accepts from Starbite Maintenance [V memory
  note]; only `hw/sbusb_desc.c`, `sbusb_ncm.c` (framing off) and three requests would change.

## 7. Identity

VID:PID **18d1:4ee0**, the loader's [V] `shim/starlte/usb_desc.h`, as the task asked; bcdDevice 0x0200 (the loader
is 0x0100) and the product string "Starbite S9 network (NCM)" tell them apart. MAC addresses are locally
administered, `02:53:42:xx:xx:xx`, derived from the USB serial by FNV-1a, so the Mac sees the same adapter on every
boot; the host's address is the device's with bit 0 of the last byte set.

**[I]** Whether reusing the loader's VID:PID makes macOS skip the "allow accessory" prompt. The memory note says the
loader's identity works unattended; it does not say what macOS keys its approval on (VID:PID alone, or also the
serial / product / interface set). PHONE-TEST.md therefore treats the first run as needing the user at the Mac.
`tools/installer/sbinst/common.py` and `steps.py` mention 4ee0: check that nothing there takes "18d1:4ee0 is on the
bus" to mean "the loader's fastboot is up" before this driver is used next to the installer.

## 8. DMA memory

One block of 0x22000 bytes (event buffer, TRBs, setup and ep0 buffers, 4 + 4 NTB buffers of 16 KiB), physically
contiguous, below 4 GiB, requested uncached (`kIOMapInhibitCache`), like the loader's non-cacheable window [V].
**[I]** that `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` honours `kIOMapInhibitCache` on our kernel. As a
second line the hw layer calls a `sync` hook around every hand-over, which the kext maps to
`performOperation(kIOMemoryIncoherentIOStore / Flush)`; with uncached memory those are harmless.
The controller takes 64-bit addresses (GEVNTADRHI, TRB word 1), but only addresses below 4 GiB have been used on
this phone [V], hence the mask.

## 9. Open questions

1. Does a boot by catalog key (no menu shown) leave the MAX77705 D+/D- switch on the AP? The loader closes it in
   `muic_route_usb()` only when its own USB comes up (`fbbg_poll` from a menu or the crash screen, or fastboot mode).
   This driver has no MUIC access (that is driver 20/22's chip). If the switch is open the PHY line state stays SE0
   and the "no host yet" log line shows it.
2. Same question for the clocks if the usb-phy nub lacks the gate entries.
3. DWC3 revision on the S9 (decides the GCTL clock-gating workaround and REQINFO): read it from the start log line.
4. macOS host NCM driver binding and the accessory prompt (sections 6 and 7).
5. IONetworkingFamily is not in our kernel today (INTEGRATION.md).
