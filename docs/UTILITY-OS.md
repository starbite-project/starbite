# Starbite Utility OS

A pocket toolbox with **no Linux** (docs/STARBITE-MASTER-PLAN.md 4.4): a bare arm64 image on `libsb`, like
Starbite Lab, started from the boot menu (catalog key `utility`, store entry `utility.img`). Version 1 covers
milestones **UO1** (USB device mode with a serial shell) and **UO2** (the phone as USB drives).

## Using it

| Thing | What it does |
|---|---|
| Launcher | Tiles **USB**, **Radio**, **Files**, **Measure**, **Everyday**, **Boot menu**. Volume moves, Power opens, tap works; Power held / Bixby / swipe right goes back |
| USB drives | Plug the phone into a computer: two drives appear. **RAM disk**: FAT32, label `STARBITE`, read-write, up to 1 GiB of DRAM bank 2 (gone when the phone restarts: copy files off first). **microSD (ro)**: the whole card, **read-only** (it holds the Linux images and backups) |
| UFS partitions | USB page → **Offer UFS partitions as drives** (or the shell's `ufs on` / `ufs off`): BOOT, SYSTEM, VENDOR, ODM, WINESP, WINDOWS, A16DATA and USERDATA become extra **read-only** drives (512-byte blocks over UFS's 4 KiB ones), to copy them to the computer (`dd` from `/dev/rdiskN`, then compare with the phone's `hash`). USB reconnects when the set changes. EFS, PARAM, KEYSTORAGE and the other never-touch partitions are never offered |
| Trackpad and keys | USB page → **Trackpad and keys** (UO3): the screen is a trackpad (drag moves the pointer, tap clicks, hold still 0.6 s right-clicks, Volume scrolls), with buttons Copy, Paste, Undo, Cmd Tab, Space, Return, Esc, Mission Control and the media keys Vol -, Play, Vol +, Mute (Mac shortcuts: Command is the HID GUI key). Bixby leaves |
| Files | **Browse the card** (read only: folders and files on the card's exFAT partition; pick a file for its SHA-256 + CRC32) and **Hash** every UFS partition or the whole card (live progress, MB/s, time left; keeps running if you leave the page) |
| Shell | A USB serial port (`/dev/cu.usbmodem*` on a Mac, 115200 8N1, any terminal): `help`, `info`, `drives` (sizes, traffic, SCSI counters), `parts` (UFS partitions and sizes), `ufs on|off`, `hash NAME [BYTES]` (SHA-256 + CRC32 of a partition or `card`, or of their first BYTES: e.g. `hash SYSTEM 6419857408` checks One UI's system image), `log`, `menu` (restart into the Starbite menu), `maint` (Starbite Maintenance), `off` |
| USB page | Connection state and speed, each drive with its traffic, SCSI command and error counters, the shell state |
| Leaving | The **Boot menu** tile (asks first), or the shell's `menu`. With **no computer ever connected and no touch or key for 10 minutes** it returns to the menu by itself (a countdown shows in the status bar for the last minute) |

| Measure | **USB charger tester** (live: input, VBUS, charge state, battery %, voltage, current in/out, power, the input and charge current limits; read only, Lab's MAX77705 readers), **Ruler** (cm on the left, inches on the right, at the panel's 570 ppi), **Colour fields** (white, black, red, green, blue, grey full screen for dead pixels; the label fades so nothing covers a pixel) |
| Everyday | **Clock** (the phone's RTC, UTC), **Stopwatch** (Power start/stop, tap to reset when stopped), **Timer** (tap +1 min, Power start/pause; flashes when done) |

Radio and Files show what comes next (master plan 4.4): file manager, hashing and imaging (UO4), then the
radio tools once bare-metal drivers exist; a calculator and the flashlight are still to come.

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
| `usb_dwc3.c`, `usb_phy.c`, `max77705_muic.c` | The loader's drivers (shim/starlte). Changed in the copy: the DMA window at 0xa7000000 (libsb's non-cacheable range) instead of 0x91000000; 10 physical endpoints instead of 8; SET_CONFIGURATION configures the descriptor file's endpoint list (`usb_desc_eps`, `usb_ep_interval`) |
| `usb_desc.c`, `usb_desc.h` | The loader's descriptors with interface 2 as mass storage (class 08/06/50) on the fastboot endpoints 0x83/0x03, and interface 3 as HID (keyboard report 1, mouse report 2 with wheel, consumer control report 3) on interrupt endpoint 0x84 (1 ms); Get Max LUN, the bulk-only reset, the HID class requests; the endpoint list the DWC3 driver configures (`usb_desc_eps`); VID:PID 1209:0001 |
| `exfat.c`, `exfat.h` | Read-only exFAT: the MBR's type 0x07 partition, the boot sector, directory entry sets (UTF-16 names), reads at any offset over FAT chains or NoFatChain runs, consecutive clusters read together |
| `hash.c`, `hash.h`, `sha256.c` | The hash job (1 MiB per service-loop step, UFS through `ufs_read_to`, the card through `sd_read4k`) and pure SHA-256 / CRC32 (zlib's) |
| `hid.c`, `hid.h`, `pad.c` | UO3: the HID report queue (pointer motion accumulated and sent in steps of at most 127), the trackpad and key screen; libsb's `ui_touch_raw` (every move an EV_DRAG, no swipes) makes the trackpad |
| `hw.h`, `fastboot.h`, `tb.c` | The few loader helpers those drivers use, on libsb |
| `build.sh` | Lab's build (same flags and checks: relocations, pre-relocation code, host paths), output `starbite/out/utility.img`; on a native aarch64 host it first runs the host test |
| `tools.c`, `ut.h` | Measure and Everyday |
| `starbite/host/ut-preview.sh` | Host preview (the fake card is the exFAT fixture): the real Utility OS and sbui code on `host.c`'s fake hardware (plus `ut-host-stubs.c` for USB), scripted keys, PNGs to `starbite/utility/preview/` (home, USB, Radio, leave dialog, Measure, ruler, colour fields, Everyday, the idle countdown); every frame is checked fully presented |
| `starbite/tools/ut-host-test.c` | Host test: a fake controller plays the host through 13 SCSI commands (write and read back across chunks, padding and residue, sense, the read-only refusal, out of range, a bulk-only reset); the FAT32 image it formats passes `fsck.fat -n`; the configuration descriptor at both speeds (length 123 = wTotalLength, the descriptor chain: 4 interfaces, 6 endpoints, the HID descriptor's report length, balanced collections) |

DMA layout in the window (`USB_DMA_BASE` 0xa7000000): the driver's event buffer, TRBs and ep0 in the first
16 KiB; the CBW at +0x8000, the CSW at +0x8400, the shell's buffers at +0x9000 / +0xa000, the drive data
(128 KiB per transfer) at +0x20000, the HID report at +0xb000.

## Status (2026-11-07)

- Built and host-tested. On S9 #1 (first unattended start): the Mac enumerated it ("Starbite Utility (Galaxy
  S9)", high speed) and read every descriptor, then held it unregistered (most likely the new-accessory
  approval). The drives and the shell were not reached yet; that build had no idle return (added since).
- Since then (not yet on the phone): Measure, Everyday, UO3 keyboard/mouse/media keys with the trackpad
  screen, Files hashing (+ shell `parts`, `hash`), the card browser, the calculator, UFS partitions as
  read-only drives, the idle return; all previewed on the host.
- Next on the phone, with the accessory allowed: UO2's check (both drives mount; a copied file's hash matches
  after an unmount and remount), the shell, then UO3 (the pointer moves, a key arrives); then UO4 (file
  manager, hashing, imaging) and the calculator.
