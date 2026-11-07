# Starbite Utility OS

A pocket toolbox with **no Linux** (docs/STARBITE-MASTER-PLAN.md 4.4): a bare arm64 image on `libsb`, like
Starbite Lab, started from the boot menu (catalog key `utility`, store entry `utility.img`). Version 1 covers
milestones **UO1** (USB device mode with a serial shell) and **UO2** (the phone as USB drives).

## Using it

| Thing | What it does |
|---|---|
| Launcher | Tiles **USB**, **Remote**, **Sensors**, **Radio**, **Files**, **Measure**, **Everyday**, **Boot menu**. Volume moves, Power opens, tap works; Power held / Bixby / swipe right goes back |
| USB drives | Plug the phone into a computer: two drives appear. **RAM disk**: FAT32, label `STARBITE`, read-write, up to 1 GiB of DRAM bank 2 (gone when the phone restarts: copy files off first). It starts with `README.TXT`, `PHONE.TXT` (serial, memory banks, the card, every UFS partition with its blocks), `PMSG.TXT` (**the previous boot's log**: the loader's lines and the OS before, from the pmsg ring) and `LOG.TXT` (this start). **microSD (ro)**: the whole card, **read-only** (it holds the Linux images and backups) |
| UFS partitions | USB page → **Offer UFS partitions as drives** (or the shell's `ufs on` / `ufs off`): BOOT, SYSTEM, VENDOR, ODM, WINESP, WINDOWS, A16DATA and USERDATA become extra **read-only** drives (512-byte blocks over UFS's 4 KiB ones), to copy them to the computer (`dd` from `/dev/rdiskN`, then compare with the phone's `hash`). USB reconnects when the set changes. EFS, PARAM, KEYSTORAGE and the other never-touch partitions are never offered |
| Trackpad and keys | USB page → **Trackpad and keys** (UO3): the screen is a trackpad (drag moves the pointer, tap clicks, hold still 0.6 s right-clicks, Volume scrolls), with buttons Copy, Paste, Undo, Cmd Tab, Space, Return, Esc, Mission Control and the media keys Vol -, Play, Vol +, Mute (Mac shortcuts: Command is the HID GUI key). **Your own buttons**: up to 8 macros from `/starbite/macros.toml` on the card replace the first two rows (below). Bixby leaves |
| Files | **Browse the card** (read only: folders and files on the card's exFAT partition; pick a file for its page: name, size, **its start as text** (the first 64 KiB, wrapped) or **as bytes** (hex, the first 8 KiB) when it is not text, and **SHA-256 + CRC32** of the whole file) and every UFS partition or the whole card: its page shows the size, the UFS blocks, **its first 8 KiB as hex** (only for what the USB drives may offer and the card: EFS, PARAM and the like hold the phone's identity) and **SHA-256 + CRC32** of all of it (live progress, MB/s, time left; keeps running if you leave the page) |
| Shell | A USB serial port (`/dev/cu.usbmodem*` on a Mac, 115200 8N1, any terminal): `help`, `info` (version, uptime, the RTC as it holds it), `drives` (sizes, traffic, SCSI counters), `parts` (UFS partitions and sizes), `ufs on|off`, `hash NAME [BYTES]` (SHA-256 + CRC32 of a partition or `card`, or of their first BYTES: e.g. `hash SYSTEM 6419857408` checks One UI's system image), `log`, `menu` (restart into the Starbite menu), `maint` (Starbite Maintenance), `off` |
| Status bar | USB state on the left; local time and battery on the right (the uptime if the RTC does not read) |
| USB page | Connection state and speed, each drive with its traffic, SCSI command and error counters, the shell state |
| Screen saver | After **2 minutes without a touch or key** the screen goes black (OLED: no burn-in, less battery) with a small dim status that moves every 30 s (USB drives, a running hash, or the countdown to the menu). A touch or key brings the page back as it was; that touch does nothing else. USB, the shell and hashing keep running. **Not while a page is watched**: Measure (and its ruler and colour fields), a running stopwatch or timer, a finished timer for 10 minutes; those also hold off the 10-minute return |
| Leaving | The **Boot menu** tile (asks first), or the shell's `menu`. With **no computer ever connected and no touch or key for 10 minutes** (counted from the Utility OS's start, not the phone's) it returns to the menu by itself (the screen saver shows the countdown) |

| Measure | **USB charger tester** (live: input, VBUS, charge state, battery %, voltage, current in/out, power, the input and charge current limits; read only, Lab's MAX77705 readers), **Ruler** (cm on the left, inches on the right, at the panel's 570 ppi), **Colour fields** (white, black, red, green, blue, grey full screen for dead pixels; the label fades so nothing covers a pixel) |
| Everyday | **Clock** (local time from the phone's RTC, which holds UTC; Helsinki with the EU summer-time rule; tap it for the **world clock**: UTC, London, Berlin, New York, Los Angeles, Tokyo), **Stopwatch** (Power start/stop, tap to reset when stopped), **Timer** (tap +1 min, Power start/pause; flashes when done), **Calculator** (scientific, in degrees, and programmer, 64-bit), **QR codes** (below) |
| Remote | The phone as the computer's input over the same USB cable (section "Utility OS 2"): **Keyboard and trackpad**, **Trackpad and macros** (the screen above), **Gamepad**, **Media remote**, **Presenter**; and **the computer's keyboard layout** (US, Finnish (Mac), Finnish (PC)) |
| Sensors | **Battery** (charge, mAh left of full, voltage, current, power, time to empty/full, design capacity, health, cycles, live charts), **Temperatures** (every TMU probe, the six thermistors, live charts), the two **hall sensors** (flip cover), the **keys** held, the **multi-touch test**; what needs power or firmware first is listed with the reason |

Radio lists the radio chips and what comes next (section "Utility OS 2"); the flashlight is still to come.

**Macro pad file** (`/starbite/macros.toml` on the card's exFAT partition, read when the trackpad screen
opens): one button per line, `"Label" = "keys"` or `"Label" = "text:what to type"`. Keys are modifiers joined
with `+` (`cmd`/`command`, `ctrl`, `alt`/`option`, `shift`) and one key (`a`-`z`, `0`-`9`, `f1`-`f12`, `enter`,
`esc`, `tab`, `space`, `backspace`, `up`/`down`/`left`/`right`, `minus`, `equal`, `comma`, `period`, `slash`).
Text (up to 47 bytes of UTF-8: everything the chosen layout can type, `äöå` and `€` on the Finnish ones; other
characters are skipped) is typed as the key presses of the layout set on the Remote page, whole even when it is
longer than the report queue. Lines starting with `#` and lines it cannot read are skipped; labels show up
to 15 characters. Without the file (or without a card) the default buttons stay.

```toml
"Copy" = "cmd+c"
"Lock" = "ctrl+cmd+q"
"Shot" = "shift+cmd+4"
"Sign-off" = "text:Best regards"
```

**First connection to a Mac**: the USB identity is new (`1209:0001`, "Starbite Utility (Galaxy S9)"), so macOS
may hold it until you allow the accessory (System Settings, Privacy & Security, "Allow accessories to
connect"). Until then the Mac shows nothing, and the phone stays in the Utility OS (leave it with the Boot menu
tile, or wait for the 10-minute return).

## Utility OS 2 (2026-11-07): the ultimate utility OS

The user's brief: keyboard features, NFC, radios, every sensor; "genuinely super powerful and diverse".
Version 2.0 is the first batch: everything that needs no power rail switched on and no firmware. Later stages
are below with what each needs; a stage that powers something up waits for a phone test with the user present.

### Batch 1 (built, host-tested, previewed; not yet on the phone)

| Screen | What |
|---|---|
| Remote: Keyboard and trackpad | A full keyboard in five rows with a trackpad above it. **Layers**: letters (the layout's own: `å`, `ö`, `ä`, `<` on Finnish), symbols (all of ASCII and €), **Fn** (Esc, F1-F12, Print Screen, Scroll Lock, Pause, Insert, Home, End, Page Up/Down, Caps Lock, Menu, forward delete, the arrows) and a **numpad** (the keypad's own keys). **Sticky modifiers** Shift, Ctrl, Alt/Option, Cmd/Win: tap once for the next key, twice to lock (a bar under the label), again to release. Held keys repeat (after 0.45 s, 14 per second). **Every finger counts**: a modifier held with one thumb works with a key under the other. A line above shows what was typed (the phone's view of it). Trackpad: drag moves, tap clicks, two fingers scroll, a two-finger tap right-clicks; Volume scrolls, Power clicks. Bixby or Power held leaves |
| Remote: Gamepad | Hold the phone sideways (its top to the left). Two sticks that centre where the thumb lands (X/Y, Z/Rz), a D-pad (the hat switch, 8 ways), A B X Y, L1 R1 L2 R2, Select, Start, Home; the volume keys are buttons 14 and 15. A standard HID gamepad (report 4: 16 buttons, hat, 4 axes): SDL, Steam, browsers and emulators see it; macOS's Game Controller framework only takes known controllers. Hold Bixby to leave |
| Remote: Media remote | Previous, Play/Pause, Next, Volume -/+, Mute, Brightness -/+, Stop, Lock (Ctrl+Cmd+Q on a Mac, Win+L on a PC), Space, Esc; the volume keys change the volume, Power plays/pauses |
| Remote: Presenter | Next and Back (the arrow keys: Keynote, PowerPoint, Google Slides, PDF viewers), Black (B), End show (Esc); **Volume Up next, Volume Down back**; the talk's time and slides forward; the screen stays on |
| Remote: the computer's layout | **US**, **Finnish (Mac)**, **Finnish (PC)** (Windows and Linux; Swedish is the same). The phone sends key positions; the layout set here decides which presses type a character (`@` is Option+2 on a Mac, AltGr+2 on a PC). Default Finnish (Mac). For the session only (no settings file yet) |
| Sensors | Battery health from the fuel gauge (FullCapRep 0x35, FullCapNom 0x23, DesignCap 0x18, Cycles 0x17 in hundredths, RepCap 0x05, TTE 0x11 / TTF 0x20 at 5.625 s) with current and voltage charts; temperatures (TMU, the thermistors) with charts; the hall sensors' pins (gpa3-6 flip cover, gpa0-2 the second; which level means "magnet" is to be seen on the phone); the keys; the multi-touch test (every finger, its trail, the most at once). The charts fill while the tile is open; a page with charts keeps the screen on |
| Everyday: QR codes | One code per line of `/starbite/qr.txt` on the card (`Label = text`; `#` comments): a Wi-Fi login (`WIFI:T:WPA;S:name;P:pass;;`), a link, a note. ECC M (L when it does not fit), up to 271 bytes. The screen stays on |
| Radio | The radio chips and buses, and what comes next (below) |

Keyboard notes for the phone test: on the first connection macOS may open the **Keyboard Setup Assistant**
(the phone is a new keyboard): choose ISO (European). On an ISO keyboard macOS may swap the key left of 1 and
the key left of Z for some keyboards; if `<` types `§`, that is it (to be seen; a swap switch can follow).

### The phone's sensors and radios (research 2026-11-07, Samsung's device tree and headers)

| Part | Chip, bus | Needs | Stage |
|---|---|---|---|
| Battery, charger | MAX77705 (fuel gauge 0x36, charger 0x69) on HSI2C 0x14360000 | nothing (libsb reads it) | **done** |
| Temperatures | TMU TOP/SUB, CMGP ADC thermistors | nothing | **done** |
| Hall sensors | gpa3-6, gpa0-2 (ALIVE GPIO) | nothing | **done** (meaning of the level to verify) |
| Grip (SAR) | Semtech SX9320, 0x28 on HSI2C 0x104C0000 (the touch block's second I2C, gpp1-6/7); 1.8 V rail always on | bring up a second I2C in the touchscreen's USI block | 2 |
| Notification LED, haptic | MAX77705 LED 0x30-0x38, haptic MCONFIG 0x10 + PWM | gated writes to the MAX77705 (`sb_max_write`) | 2 |
| Torch | S2MPB02 flash LED (0x59 on HSI2C 0x10920000), 20 mA code | a write to the camera PMIC | 2 |
| Heart rate, SpO2 | MAX86915, 0x57 on HSI2C 0x108E0000 (USI08); LED boost gpg1-6 | **two rails**: S2MPB02 LDO8 and S2MPS18 LDO38 (ACPM) | 3 |
| NFC | Samsung sec-nfc (probably S3NRN82), 0x27 on the same bus; IRQ gpa1-2, CLK_REQ gpa2-6; VEN and FIRM on the PCAL6524 expander (0x22 on 0x10930000, which also holds BT_REG_ON) | LDO38, the PMU clock out, VEN; firmware stays in the chip; a vendor RF configuration may be needed | 3 |
| Accelerometer, gyro, compass, barometer, light, proximity | inside the **BCM4775** GNSS chip's sensor hub, SPI 0x13AC0000 (USI_CHUB00); its rail is always on | its firmware (413,864 bytes) is compiled into Samsung's kernel: read from the stock BOOT image on the phone at run time, never committed; four protocol layers | 4 |
| Wi-Fi scan | BCM4361 on PCIe; firmware from VENDOR | the `wifi-core` session's portable core | 4 |
| Bluetooth LE scan | BCM4361 on UART 0x10840000; BT_REG_ON on the expander | after Wi-Fi | 5 |
| Fingerprint, iris, GPS | Egis ET510 (TrustZone owns the SPI), the iris IR LED (eye safety), BCM4775 GNSS (closed protocol) | — | **never** |
| Modem | Shannon | — | **never transmits** |

With stage 4: a **level** and **compass** (accelerometer, magnetometer), **altitude** (barometer), a
**vibration meter**, **lux** and **proximity**; stage 3: **heart rate** and an **NFC tag reader** (type, UID,
NDEF text and links of your own tags; never payment cards); stage 2: the **torch**, the **LED** and
**vibration** tests, the **grip** sensor. More tools for later batches: a USB serial/network adapter (CDC
ACM to a second port, then NCM), a local on-screen keyboard for notes and QR text, settings on the card
(`/starbite/utility.toml`: layout, ...), a sound meter once audio exists.

### Rules (unchanged)

Read only towards the phone's identity areas; no UFS writes beyond what version 1 does (none). Nothing in
batch 1 writes a PMIC or the MAX77705: the new screens read registers and send USB reports. Stages 2-3 use
libsb's gated writers only, with the user present for the first run.

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
| `tz.c`, `tz.h`, `clock.c` | Time zones (civil dates, the EU rule, the US rules since 1987; checked against Python's zoneinfo for 2000-2099), the status bar's time and battery (the RTC read once a minute, the fuel gauge every 10 s), the world clock page |
| `macros.c`, `macros.h` | The macro pad file's parser (pure C; the host test parses examples and the fixture's file) |
| `hw.h`, `fastboot.h`, `tb.c` | The few loader helpers those drivers use, on libsb |
| `build.sh` | Lab's build (same flags and checks: relocations, pre-relocation code, host paths), output `starbite/out/utility.img`; on a native aarch64 host it first runs the host test |
| `tools.c`, `ut.h` | Measure, Everyday, Files (the card browser; `ut_card` gives other screens the card's exFAT) |
| `remote.c`, `kbd.c`, `gamepad.c` | The Remote page, the media remote and the presenter; the keyboard with its trackpad; the gamepad (section "Utility OS 2") |
| `kbdmap.c`, `kbdmap.h` | Character to key presses for the US, Finnish (Mac) and Finnish (PC) layouts (pure C, host-tested) |
| `mt.c`, `mt.h` | Every finger (libsb `sb_touch_events`) and the raw keys for the screens that need two thumbs; restarts the idle timer |
| `text.c` | Text with å ä ö Å Ä Ö, € and ¤ drawn over the font's base letter (menu v7's fonts are ASCII and a few symbols) |
| `sensors.c` | The Sensors tile (read only) |
| `qr.c`, `qr.h`, `qrpage.c` | QR encoder (byte mode, versions 1-10, all ECC levels and masks) and the QR codes screen |
| `starbite/host/ut-preview.sh` | Host preview (the fake card is the exFAT fixture): the real Utility OS and sbui code on `host.c`'s fake hardware (plus `ut-host-stubs.c` for USB), scripted keys, PNGs to `starbite/utility/preview/` (home, USB, Radio, leave dialog, Measure, ruler, colour fields, Everyday, the idle countdown); every frame is checked fully presented |
| `starbite/tools/ut-host-test.c` | Host test: a fake controller plays the host through 13 SCSI commands (write and read back across chunks, padding and residue, sense, the read-only refusal, out of range, a bulk-only reset); the FAT32 image it formats passes `fsck.fat -n`; the configuration descriptor at both speeds (length 123 = wTotalLength, the descriptor chain: 4 interfaces, 6 endpoints, the HID descriptor's report length, balanced collections); exFAT against the fixture card (MBR, a fragmented file, a deep path); the calculator; the macro parser; the keyboard layouts (each types all of printable ASCII, Finnish also ÄÖÅ and €, no two characters share a key press, dead keys add a Space, keys within the report's range), Finnish text through the report queue, the input report sizes per report ID against what hid.c sends, the gamepad report (sent once, ahead of queued keys) |
| `starbite/tools/qr-test.c`, `qr-check.py` | QR host test: 786 symbols (versions 1-10, ECC L/M/Q/H, all 8 masks; finder, timing, format and version bits read back); `qr-check.py` decodes every one with `zbarimg` (Lima VM) and compares the bytes |

DMA layout in the window (`USB_DMA_BASE` 0xa7000000): the driver's event buffer, TRBs and ep0 in the first
16 KiB; the CBW at +0x8000, the CSW at +0x8400, the shell's buffers at +0x9000 / +0xa000, the drive data
(128 KiB per transfer) at +0x20000, the HID report at +0xb000.

## Status (2026-11-07)

- Version 2.0 (batch 1 of "Utility OS 2"): built and host-tested, previews of every new screen; waiting for
  its phone test.

- Built and host-tested. On S9 #1 (first unattended start): the Mac enumerated it ("Starbite Utility (Galaxy
  S9)", high speed) and read every descriptor, then held it unregistered (most likely the new-accessory
  approval). The drives and the shell were not reached yet; that build had no idle return (added since).
- Since then (not yet on the phone): Measure, Everyday, UO3 keyboard/mouse/media keys with the trackpad
  screen, Files hashing (+ shell `parts`, `hash`), the card browser, the calculator, UFS partitions as
  read-only drives, the idle return, the macro pad file, the screen saver, the file viewer, local time and
  battery in the status bar, the world clock; all previewed on the host.
- Next on the phone, with the accessory allowed (`build/ut-mac-test.py`, then `--ufs`): UO2's check (both
  drives mount; a copied file's hash matches after an unmount and remount), the shell (`info`: the build and
  the RTC against the Mac's clock), macOS listing the HID devices, the UFS partitions as drives (macOS asks
  about the unreadable ones: Ignore); by hand: the trackpad and keys, the card files, the screen saver.
