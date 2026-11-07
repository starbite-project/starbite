# Starbite Utility OS

A pocket toolbox with **no Linux** (docs/STARBITE-MASTER-PLAN.md 4.4): a bare arm64 image on `libsb`, like
Starbite Lab, started from the boot menu (catalog key `utility`, store entry `utility.img`). Version 1 covers
milestones **UO1** (USB device mode with a serial shell) and **UO2** (the phone as USB drives).

## Using it

| Thing | What it does |
|---|---|
| Launcher | Tiles **USB**, **Radio**, **Files**, **Measure**, **Everyday**, **Boot menu**. Volume moves, Power opens, tap works; Power held / Bixby / swipe right goes back |
| USB drives | Plug the phone into a computer: two drives appear. **RAM disk**: FAT32, label `STARBITE`, read-write, up to 1 GiB of DRAM bank 2 (gone when the phone restarts: copy files off first). **microSD (ro)**: the whole card, **read-only** (it holds the Linux images and backups) |
| Shell | A USB serial port (`/dev/cu.usbmodem*` on a Mac, 115200 8N1, any terminal): `help`, `info`, `drives` (sizes, traffic, SCSI counters), `log`, `menu` (restart into the Starbite menu), `maint` (Starbite Maintenance), `off` |
| USB page | Connection state and speed, each drive with its traffic, SCSI command and error counters, the shell state |
| Leaving | The **Boot menu** tile (asks first), or the shell's `menu`. With **no computer ever connected and no touch or key for 10 minutes** it returns to the menu by itself (a countdown shows in the status bar for the last minute) |

Radio, Files, Measure and Everyday show what comes next (master plan 4.4): file manager, hashing and imaging
(UO4), charger tester and screen tools, clock and calculator, then the radio tools once bare-metal drivers exist.

**First connection to a Mac**: the USB identity is new (`1209:0001`, "Starbite Utility (Galaxy S9)"), so macOS
may hold it until you allow the accessory (System Settings, Privacy & Security, "Allow accessories to
connect"). Until then the Mac shows nothing, and the phone stays in the Utility OS (leave it with the Boot menu
tile, or wait for the 10-minute return).

## Safety

- Never writes UFS. The card is exposed read-only (the SCSI write-protect bit; writes are refused with DATA
  PROTECT). The RAM disk lives at 0x880000000 (DRAM bank 2), below any reserved-memory region the device tree
  lists there.
- It feeds the cluster-1 watchdog that the loader arms for this entry (catalog `wdt = 60`, `wdt_feed = "own"`):
  a hang resets into the loader's crash screen.

## Inside

| File | What |
|---|---|
| `starbite/utility/main.c` | Startup (UI, touch, drives, USB), the service loop (USB events, the shell, the watchdog, the idle return; also run from every UI loop through `ui_tick`), the launcher and pages |
| `msc.c`, `msc.h` | USB Mass Storage, Bulk-Only Transport 1.0, SCSI (INQUIRY with VPD 0/80, TEST UNIT READY, REQUEST SENSE, READ CAPACITY 10/16, READ FORMAT CAPACITIES, MODE SENSE 6/10, READ/WRITE 10/12/16, SYNCHRONIZE CACHE, VERIFY, START STOP, PREVENT ALLOW). Never stalls an endpoint: short data is padded and the CSW residue says how much was real (BOT 6.7) |
| `fat.c`, `fat.h` | FAT32 formatter for the RAM disk (no partition table, 4 KiB clusters) |
| `usb_dwc3.c`, `usb_phy.c`, `max77705_muic.c` | The loader's drivers (shim/starlte), copied unchanged except the DMA window: 0xa7000000 (libsb's non-cacheable range) instead of 0x91000000 |
| `usb_desc.c`, `usb_desc.h` | The loader's descriptors with interface 2 as mass storage (class 08/06/50) on the fastboot endpoints 0x83/0x03, so the DWC3 driver configures the same five endpoints; Get Max LUN and the bulk-only reset; VID:PID 1209:0001 |
| `hw.h`, `fastboot.h`, `tb.c` | The few loader helpers those drivers use, on libsb |
| `build.sh` | Lab's build (same flags and checks: relocations, pre-relocation code, host paths), output `starbite/out/utility.img`; on a native aarch64 host it first runs the host test |
| `starbite/tools/ut-host-test.c` | Host test: a fake controller plays the host through 13 SCSI commands (write and read back across chunks, padding and residue, sense, the read-only refusal, out of range, a bulk-only reset); the FAT32 image it formats passes `fsck.fat -n` |

DMA layout in the window (`USB_DMA_BASE` 0xa7000000): the driver's event buffer, TRBs and ep0 in the first
16 KiB; the CBW at +0x8000, the CSW at +0x8400, the shell's buffers at +0x9000 / +0xa000, the drive data
(128 KiB per transfer) at +0x20000.

## Status (2026-11-07)

- Built and host-tested. On S9 #1 (first unattended start): the Mac enumerated it ("Starbite Utility (Galaxy
  S9)", high speed) and read every descriptor, then held it unregistered (most likely the new-accessory
  approval). The drives and the shell were not reached yet; that build had no idle return (added since).
- Next: with the accessory allowed, UO2's check (both drives mount; a copied file's hash matches after an
  unmount and remount), then UO3 (keyboard, mouse, macro pad) and UO4 (file manager, hashing, imaging).
