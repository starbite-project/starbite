# Starbite master plan: the locked lineup

Written 2026-11-03. It is built from six research reports (Apple, Google, Microsoft, Linux, Starbite's own OSes, Installer and storage) and from the repo (NOTES.md, docs/*.md, shim/starlte, tools/*).

- This file replaces the order of work in `docs/STARBITE-2-PLAN.md` §8.
- That file's §0 (the locked lineup and the parked list) still stands and is repeated in §1.1.
- Nothing here has been done yet unless it says **works today** or **built**.
- **Built** means the files exist; it does not mean they were booted. **Booted** does not mean the OS works.

**How to read this.** §0 is the one-page summary. §7 is the order of work. §8 lists what you need to decide. Everything else is detail you can look up when we get there.

**Tags used**

| Tag | Meaning |
|---|---|
| **[unverified]** | Needs a test on the phone, or a check, before we rely on it |
| **[est]** | An estimate, not a measurement |
| **[claim]** | Another project says so; we have not checked it ourselves |
| `file:line` | Points into this repo |

**Effort scale** (rough, used everywhere in this file)

| Size | Meaning |
|---|---|
| **S** | about 1 session, 1-3 phone boots |
| **M** | about 2-5 sessions |
| **L** | about 6-15 sessions |
| **XL** | more than 15 sessions, or research that may never finish |

A **session** is one evening of focused work with the phone, a few hours long. One iOS test boot (flash, reboot, pull the log) takes about 8 minutes today. These sizes are for planning. They are not promises.

**Short glossary**

| Word | One line |
|---|---|
| S-Boot | Samsung's signed bootloader. It loads whatever is in the BOOT partition. We cannot change it |
| Download mode | S-Boot's flashing mode (Vol Down + Bixby + Power). It can only write partitions; it cannot run our code |
| BOOT / RECOVERY | The two partitions S-Boot starts. Our loader lives in BOOT; TWRP lives in RECOVERY today |
| Loader | Starbite's own boot program in BOOT: the boot menu, Engineer Lite, and the code that starts each OS |
| Store | Our own container of boot files (kernels, payloads, UEFI images). One on UFS, one on the card |
| Catalog | The list of menu entries in the store (`tools/store/catalog.toml`). Adding an OS = adding an entry |
| UFS | The phone's internal 64 GB flash |
| GPT / PIT | The internal partition table / Samsung's copy of it for Download mode. Ours no longer match past SYSTEM |
| EFS | The partition with the phone's IMEI and radio calibration. Unique to each phone; losing it is permanent |
| XNU | Apple's kernel. We build our own (xnu-12377) for the S9 |
| EL1 / EL2 | CPU privilege levels. We get EL1. Samsung's hypervisor (uH/RKP) keeps EL2, so no virtual machines here, ever |
| A55 / M3 | The phone's two CPU types: 4 small Cortex-A55 (ARMv8.2) and 4 big Mongoose M3 (ARMv8.0, missing newer instructions) |
| PIO / IDMAC / UHS | microSD speed steps: CPU-copied (slow), DMA (faster), 1.8 V ultra-high-speed (fastest) |
| GSI | Google's Generic System Image: an Android system that runs on any phone's vendor part |
| Vendor | The phone-specific half of Android (drivers, HALs). Ours comes from krazey90's LineageOS 24 |
| Mesa / Panfrost / PanVK | Open-source GPU drivers for our Mali-G72 (OpenGL / Vulkan) |
| OTG / USB host | The phone acting as the computer, so it can read a USB stick |
| DWC3 | The phone's USB controller |
| CP | The modem processor (Samsung Shannon), inside the SoC |
| TWRP | The recovery we use today to write partitions from the Mac over adb |

---

## 0. One-page summary

### 0.1 The lineup: today, target, effort, first step

| Category | OS | Status today | Target | Effort | First milestone |
|---|---|---|---|---|---|
| Apple | **iOS 26** | **Works** (Beta). Metal on the Mali, 8 cores, about 10-24 fps. Missing: wallpaper, Control Center glass, widgets, audio, Wi-Fi | Beta, root moved to the microSD, glass fixed | Card move **M**; fixes **L** | **SD1**: the microSD probe boot (payload already built) |
| Apple | **macOS** | Not started. Our kernel is already a macOS-config kernel | Experimental desktop you can tap through | **XL** (80-150 boots [est]); go/no-go at WindowServer | **MAC0**: card writes + instruction census + card slot |
| Google | **Pure Android 17** | LineageOS 24 (Android 17) **boots** on UFS. No AOSP yet | Daily: AOSP 17 system on krazey90's vendor, 24 GB data; later our own build | GSI **M**; own build **L** | **AN0**: offline SELinux compile check |
| Google | **ChromeOS** | Not started | Beta: Google's own ChromiumOS arm64 build on our kernel, with GPU; then real ChromeOS | ChromiumOS **M**; real ChromeOS **L** | **CR0**: build Mesa + minigbm, kernel options |
| Microsoft | **Windows 11** | **Works** (Experimental). Desktop, touch, UFS, 3.4 GB RAM, 4 A55 cores, software drawing | Beta: 7-8 cores, USB, keys, battery, thermal guard; later GPU | **M** per item; GPU **L-XL** | **W0**: read-only ACPI/BCD dump; **W1**: block Windows Update |
| Microsoft | **Windows 10 ARM64** | Not started | Experimental: build 19045 in a VHDX, all cores | **L-XL** (per-build firmware match) | **W6-T0**: boot a VHDX with today's UEFI, read the log |
| Linux | **postmarketOS** | **Works** (Beta). Phosh, 8 cores, Wi-Fi, USB networking, Panfrost GPU. UI slow; no BT, audio, sensors | Smooth GPU, BT, battery, haptics, brightness; later audio, rotation, camera | Basics **M**; audio/sensors/camera **L** each; modem **XL** | **P0** measure, then the **K2** kernel batch |
| Linux | **Ubuntu Touch** | Not started (an older desktop-Lomiri experiment was never built) | Beta: Ubuntu Touch 24.04-2.x without Halium, on our kernel | **M-L** | **UT1**: Lomiri smoke test on pmOS |
| Starbite | **Emergency OS** | Not started | v1 without modem: light, SOS strobe, Medical ID, battery, starts in ~3 s. v2: SMS | v1 **L**; v2 **XL** | **E1**: image in BOOT, home screen in under 3.5 s |
| Starbite | **Utility OS** | Not started | v1: phone as USB drive (RAM or card), keyboard/mouse, serial; small tools | v1 **L** | **UO1**: the phone shows up on the Mac as a USB serial port |
| Starbite | **Engineer OS** | About 70 % exists as EngineerMode inside the loader (`em.c`, 4287 lines) | Full bench: tests, stress, armed mode, Starbite manager, installer media | v1 **M-L**; v2 **L-XL** | **EN1**: today's pages running on the Starbite core |
| Starbite | **Installer** | Not started | A stock S9 becomes full Starbite from one drive | v0 (microSD) **L**; v1 (USB) **M-L** more | **IN1**: installer reads a microSD manifest on S9 #2 |
| — | **Menu v7** | Runs on the phone; too slow. Speed round 3 part 1 is **built, not flashed** | 30 fps on scroll and section expand | **M** | **MV1**: flash the pending pair, read per-class timings |

### 0.2 The plan in ten lines

1. **The critical path** is storage: the XNU microSD driver (read, then write), then the card re-layout, then iOS 26's root moves to the card, then the UFS re-layout. That last step gives Android its 24 GB and Windows 10 its room.
2. **Buy a big USB drive first** (256 GB or more, the "master drive"). The Mac has 7.4 GB free, the card has no free space, and the backups and the installer need a home.
3. **S9 #2 takes every risky storage test first.** S9 #1 stays the daily phone.
4. **The Starbite core library** comes first. Engineer OS runs on it next, then Emergency OS, then Utility OS.
5. **The Installer is a small Linux program started by our loader.** USB host mode exists as a Linux port and nowhere else. The microSD version comes first, the USB-drive version second.
6. **Android:** Google's AOSP 17 GSI on krazey90's vendor first (card test, then UFS). Our own AOSP build later, on a rented Linux machine.
7. **ChromeOS:** Google's own public ChromiumOS arm64 build first (no build machine needed), then real ChromeOS from an official recovery image.
8. **Windows:** prove 7 cores on today's Windows 11 using the M3 fix we already found for iOS. Then Windows 10 in a VHDX. The real blocker is the per-build firmware match, not the cores.
9. **Linux:** one big kernel flash (the GPU page-table fix from the Tab Active3 port, Bluetooth, audio, sensors and camera cores), then drivers as modules from the card. Ubuntu Touch without Halium.
10. **macOS:** our own kernel plus a macOS 26.5 root on the card. Go/no-go at WindowServer, which refuses to start without a Metal device. No EL2 or Snapintosh-style route.

### 0.3 "The Snapdragon thing": what we found

We did not know which Snapdragon item you meant, so we searched all four meanings.

| What | What it is | What it means for us |
|---|---|---|
| **Snapintosh** (macOS 27 on a Snapdragon X2 Elite laptop, open-sourced 2026-10-22, MIT) | An EL2 hypervisor that lets Apple's stock macOS kernel run on an ASUS Zenbook A16. A Mac must drive every boot over USB | **Its method cannot work on the S9**: Samsung's hypervisor owns EL2 here, and Apple's stock arm64e kernel would not run on our cores anyway. **We don't need it**: we build XNU from source and it already runs at EL1. We borrow its MIT pieces: the device-tree completion list, its GPU-driver interface (IOGPU/Metal plug-in headers), and later its AIR shader front end (§3.2) |
| **Snapdragon Galaxy S9** (SM-G9600, SDM845) mainline Linux work by Dzmitry Sankouski, 2024-2025 | Upstream Linux drivers for the MAX77705 power chip and the S6E3HA8 panel, which our Exynos S9 shares | **Already in our Linux 7.1 tree**: battery and charging come from it today; haptics, LED and the panel driver only need switching on. Its modem, GPU, audio and camera are Qualcomm-only and do not transfer (§3.9) |
| **Snapdragon S9 Windows** (Renegade / edk2-msm) | Windows with GPU acceleration on Snapdragon phones | Works only because Qualcomm ships Adreno GPU drivers for Windows. Nothing transfers to our Exynos/Mali phone (§3.8) |
| **Googlebook** (launched 2026-10-04; Snapdragon X Elite models) and its OS (Aluminium, "Googlebook OS") | Google's new Android-based laptop OS. Closed source | **Not an option.** Only a leaked build exists; it reportedly needs ARMv8.2 cores and a 64-bit-only vendor, and its Linux terminal needs EL2. Classic ChromeOS keeps shipping ARM images, so our ChromeOS plan stands (§3.5) |

### 0.4 What is honestly hard (read before you get your hopes up)

- **No virtual machines on this phone, ever.** Samsung's hypervisor owns EL2. So: no Android apps inside ChromeOS, no Linux apps in ChromeOS, no Snapintosh route, no Windows virtualization features.
- **macOS has 1.8 GiB of RAM to work with** (a Mac has 8 GB). It may boot to a desktop; it will not be smooth.
- **Windows has no Mali GPU driver anywhere.** A GPU-drawn Windows desktop is a long research project.
- **Calls from our own stacks (Linux, Starbite OS) are research.** Android's Samsung modem software is the only near path to calls [unverified on our Android 17 boot].
- **GPS is blocked.** It speaks a closed Broadcom protocol.
- **USB host mode is unproven on our unit.** The Installer's USB-drive version depends on it.
- **The microSD is slow today** (high-speed mode 22.6 MB/s in Linux, about 10-12 MB/s in XNU's first driver). iOS 26 on the card will feel slower until the faster modes land.

---

## 1. Principles and rules

### 1.1 Focus: the locked lineup (your words, 2026-11-03)

Two systems per category, plus Starbite's own three, plus the Installer.

| Category | Systems |
|---|---|
| **Apple** | iOS 26 (today's port) · **macOS** (on our XNU 12377 port, which is already a macOS-config kernel) |
| **Google** | **Pure Android 17** (AOSP, Pixel-like, not LineageOS; later maybe the newest One UI) · **ChromeOS** (the real thing; ChromiumOS from Google's source first; never FydeOS) |
| **Microsoft** | Windows 11 (our own EDK2 UEFI) · Windows 10 ARM64 (all 8 cores) |
| **Linux** | Ubuntu Touch · postmarketOS |
| **Starbite** | **Emergency OS** · **Utility OS** · **Engineer OS** |
| Also | **Starbite Installer**, made by the Engineer OS |

**Parked** (hidden, nothing deleted, no work on it): iOS 14, iPadOS, tvOS, iOS 7, the Darwin console, Android GSIs of other versions, LineageOS as the visible Android, Ubuntu (Phosh/GNOME/Lomiri desktop), Arch, Kali, Debian, Fedora, WinPE, the UEFI Shell (stays reachable from Engineer OS), retro and media.

Menu v7 keeps getting faster in the background until it is smooth on the phone.

### 1.2 Device safety rules (never broken)

1. **Never touch** EFS, CPEFS, the boot LUNs, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA, DTB/DTBO or the GPT/PIT region without a backup-and-verify step. **The boot LUNs (S-Boot, CM, ECT): never at all.**
2. **No Odin or Heimdall writes by name past SYSTEM.** The PIT no longer matches our GPT there.
3. **Never touch the MIF/INT clocks** (memory bus). Asking for them froze the phone with a green screen (`NOTES.md:1676`). Our code must not even be able to build that request.
4. **Never test real emergency calls.** Never dial an emergency number in any test. Never send an SOS to anyone who was not warned first.
5. **You only press buttons.** We run every command, flash, pull and analysis.
6. **Every write:** backup first, then write, then read back and compare the SHA-256. Irreplaceable data (EFS and friends) lives in at least two places off the phone.
7. **One storage-writing track per phone at a time.** Any new tool that changes the UFS partition table or UFS contents outside our stores runs on **S9 #2 first**. Card work may run on S9 #1, but only after the card's contents are verified on the master drive, and a new card-layout tool is first rehearsed on the spare 64 GB card.
8. **Windows Update stays blocked by policy** before Windows ever sees a network. An update replaces the files our firmware patches, and Windows would stop booting.
9. **The Mac's internal disk is not a staging area** (7.4 GB free). Big files stream straight to the master drive or are made on the phone.

### 1.3 How each boot is run (from your standing instructions)

- Before each boot: what changed, what to expect, the likely roadblock, and the plan if it fails.
- After each boot: what happened, what the logs say, what comes next. No silent cycles.

### 1.4 Legal limits (personal use only)

- **Apple, Microsoft, Samsung and Google binaries are never redistributed.** iOS and macOS images are a personal experiment (Apple licenses them for Apple hardware only). Each phone needs its own Windows licence.
- **Google's Android 17 GSI** is licensed "solely for testing applications" and may not be copied (except as a backup), modified or distributed. The installer must not carry it; the phone downloads it for you.
- **ChromeOS recovery images and One UI** are proprietary: same rule.
- **Radio tools never jam, never flood, never change the IMEI** (a criminal offence in many countries), never change transmit power or the regulatory domain, never imitate a base station, never store other people's traffic (§4.4).
- **No backups of one phone are ever restored onto another.** Copying S9 #1's EFS to S9 #2 would duplicate its IMEI, which is illegal.

### 1.5 Where the reports disagreed, and what this plan chose

| # | Question | Options in the reports | This plan's choice, and why |
|---|---|---|---|
| 1 | Card layout | Three different proposals (Apple, Google, Installer) | One consolidated map (§2.4). It fits iOS 26, macOS, the Android test slot, ChromiumOS and both Linux images |
| 2 | UFS layout after the move | 1 GiB USERDATA + 27.59 GiB Windows, or a 256 MiB dummy USERDATA + a 2 GiB SBSTORE + 26.34 GiB Windows | The second. Old iOS payloads write into "USERDATA" by name; a zero-filled dummy makes them fail safely instead of hitting Android's data (§2.7) |
| 3 | Installer runtime | Bare metal in the loader, or Linux started by the loader | Linux first: USB host already exists as a Linux port. Bare metal later, through the Starbite core |
| 4 | What to flash in Download mode | BOOT only, or BOOT + RECOVERY | Both. S-Boot may boot RECOVERY once after a Download-mode flash (`NOTES.md:128`) |
| 5 | Installer drive format | FAT32 + a raw store partition, or exFAT + chunked image files | exFAT + chunked `.sbi` files. Any computer can copy it, Linux reads it natively, and no file exceeds 1 GiB, so FAT32 also works |
| 6 | Where Emergency OS lives | A bundle in BOOT, or a squashfs in the store | A bundle in BOOT (bare metal, 2-4 MB), with copies in both stores. It must start even when the UFS store is broken |
| 7 | Where Engineer OS lives | BOOT, or the stores | The full Engineer OS in the stores. Today's EngineerMode stays in BOOT as **Engineer Lite**, the rescue tool |
| 8 | ChromeOS root | Its own 16 GiB card partition, or files on the card's exFAT | Files on exFAT. It matches our Linux initrd and avoids ChromeOS finding its data partition by number |
| 9 | Using 4 cores to draw the menu | Proposed for speed | Only in the Starbite OSes. The menu must hand the phone to another OS, and a core cannot be switched off cleanly here (`docs/MENU7-PORT.md` §2.4) |
| 10 | Effort scales | Days, weeks, boots, sessions | One scale (top of this file) |

---

## 2. Storage map

### 2.1 Where everything lives today

| Place | Size | Holds today |
|---|---|---|
| BOOT (UFS) | 55 MiB, about 30 MB free | Loader, menu assets, EngineerMode, the iOS 26 payload, the Windows UEFI bundle |
| RECOVERY (UFS) | 65 MiB | TWRP 3.7.0 |
| UFS user area (layout A, below) | 53.09 GiB | iOS 26 root and UFS store, WINESP, WINDOWS, A16DATA |
| UFS SYSTEM / VENDOR / ODM | 4.75 GiB / 725 MiB / 700 MiB | LineageOS 24 (Android 17) |
| 128 GB SanDisk card (S9 #1) | 119.21 GiB | p1 exFAT "STARBITE" 87.2 GiB (pmOS image, backups, windows/), p2 raw store 32 GiB. **No free space** |
| 64 GB card | — | Spare copy of the Windows VHDX; the iOS 16 root (its **only** copy) |
| 64 GB PNY USB stick | 64 GB | Archive: old images, backups (`build/ARCHIVED-TO-USB.txt`) |
| Mac | 7.4 GB free | Repo, tools, the lima VM |

### 2.2 UFS: before and after

UFS LU0 has 15,616,000 blocks of 4 KiB (59.57 GiB). Partitions 1-24 are Samsung's and end at block 1,698,432 (6.48 GiB). They are never changed. **53.09 GiB (13,917,563 blocks) is ours to lay out.**

**Layout A: today on S9 #1 (proven)**

| # | Name | 4K blocks | Size | Holds |
|---|---|---|---|---|
| 18 / 19 / 20 | SYSTEM / VENDOR / ODM | stock | 4.75 GiB / 725 MiB / 700 MiB | Android 17 (LineageOS 24 today) |
| 25 | USERDATA | 1698432..6613759 | 18.75 GiB | iOS 26 root (HFSX, 0-16 GiB, 14 GiB used) + the UFS store at +16 GiB |
| 26 | WINESP | 6613760..6744831 | 512 MiB | Windows boot files (FAT32) |
| 27 | WINDOWS | 6744832..13518591 | 25.84 GiB | Windows 11 (NTFS), 13.86 GiB used |
| 28 | A16DATA | 13518592..15615994 | 8 GiB | Android data (f2fs) |

**Layout B: the target** (once iOS 26 runs from the card and the couplings in §2.7 are fixed)

| # | Name | 4K blocks | Size | Holds |
|---|---|---|---|---|
| 25 | USERDATA | 1698432..1764095 | 256 MiB | **Dummy, zero-filled.** A retired name, so old iOS payloads fail safely |
| 29 | SBSTORE | 1764096..2288383 | 2 GiB | UFS store v2 (kernels, payloads, UEFI images, Starbite OSes) |
| 26 | WINESP | 2288384..2419455 | 512 MiB | Boot files for Windows 11 and 10 |
| 27 | WINDOWS | 2419456..9324287 | 26.34 GiB | Windows 11 + a fixed Windows 10 VHDX |
| 28 | A16DATA | 9324288..15615994 | **24.00 GiB** | Android 17 data (f2fs) |

- Blocks add up to 13,917,563. Android data is last, so a phone with a slightly different UFS size only changes A16DATA.
- Partition numbers keep S9 #1's name-to-number mapping, and WINESP/WINDOWS keep their GUIDs, so Windows' boot configuration stays valid.
- The new Windows write fence would be exactly blocks 2288384..9324287.

### 2.3 Your "24 GB" decides how much room Windows gets

| A16DATA | Starts at block | WINDOWS gets | Windows 11 (12-14 GiB after cleanup) + Windows 10 VHDX (10 GiB) + 2 pagefiles (2 GiB) leaves |
|---|---|---|---|
| 24 GiB (binary) | 9324288 | 26.34 GiB | about 0.3-2.3 GiB: **too tight** |
| **24 GB (decimal) = 22.35 GiB** | 9756416 | **27.99 GiB** | about 2-4 GiB: workable |
| 20 GiB | 10372864 | 30.34 GiB | about 4-6 GiB: comfortable |

**Recommendation: 24 GB decimal (22.35 GiB).** It is literally what you asked for and gives Windows enough room. Both Windows versions fit only with Compact OS, reserved storage off, hibernation off and fixed 1 GiB pagefiles [est].

### 2.4 The 128 GB card: before and after

**Today:** p1 exFAT 87.2 GiB (sectors 8192..182886399), p2 store 32 GiB (182886400..249999359). No free space. Probe tests use the card's last 64 MiB (sector 249868288 onward), which is the unused tail of p2.

**Two ways to change it**

| Step | What | Needs | Gives |
|---|---|---|---|
| **Quick** (ANDROID-PLAN §5.1) | Shrink the store p2 to 8 GiB; put an extended partition in the freed 24 GiB tail. p1 untouched | Nothing new | 24 GiB: room for **either** the iOS 26 root **or** the Android test slot, not both |
| **Full** (recommended) | Copy everything off p1, rewrite the whole card map, copy the OS files back | The master drive (there is no exFAT shrink tool, and the Mac has no room) | Room for everything below |

**After the full re-layout** (MBR with logical partitions: our loader's `sd.c`, XNU's `sdroot=pN` and ANDROID-PLAN already parse this; GPT would need new parsers)

| Part | Type | Size | Holds |
|---|---|---|---|
| p1 | 0x07 exFAT "STARBITE" | 36 GiB | `/distros/pmos.img` 8 GiB, `/distros/ut.img` 8 GiB (Ubuntu Touch), ChromiumOS set 13.5 GiB (root 5, overlay 0.5, state 8), kernel modules and tools 2 GiB, about 4 GiB staging for downloads |
| p2 | 0xDA store | 8 GiB | Card store v2 |
| p3 | 0x05 extended | 75.2 GiB | Container for the logical partitions below |
| p5 | 0xAF IOS26 | 20 GiB | iOS 26 root (the 16 GiB volume, its 4 MiB log ring, room to grow) |
| p6 | 0xAF MACOS | 32 GiB | macOS root [est size; created now so the numbering never changes, left empty until MAC0] |
| p7-p11 | 0x83 Google slot | 17.4 GiB | Android test: system 4.5 GiB, vendor 700 MiB, odm 160 MiB, metadata 64 MiB, data 12 GiB. After Android moves to UFS: the One UI slot or real ChromeOS |
| free | — | about 5.8 GiB | Ends with the 64 MiB test range at sector 249868288, kept free for driver tests |

- Exact sectors are computed by the layout tool at execution time (1 MiB aligned) and read back.
- Type 0x83 keeps Android's vold away from the Android slot. Type 0xAF is HFS.
- The Android slot's partition numbers differ from ANDROID-PLAN §5 (it used p5-p9). The vendor copy's fstab must follow.
- **Parked images do not fit on this card.** They live on the master drive or the 64 GB card.
- **S9 #2 with the 64 GB card** (if you agree): p1 16 GiB, p2 2 GiB, IOS26 20 GiB, Google slot 17.4 GiB, no macOS.

### 2.5 Every OS: where it boots from and where it lives (after both re-layouts)

| OS | Boot files | Root and data | Size |
|---|---|---|---|
| iOS 26 | BOOT payload (copy in SBSTORE) | Card p5 IOS26 | 14 GiB used in a 16 GiB volume |
| macOS | Store `macos.xnu` | Card p6 MACOS (HFSX) | 20-30 GB uncompressed [est]; file compression might halve it [unverified] |
| Android 17 | SBSTORE `android.kernel/ramdisk/cmdline` (Linux 4.9) | UFS SYSTEM (GSI 1.9 GB, or 3.7 GB with Google apps) + VENDOR + ODM + A16DATA | 6.2 GiB + 22.35-24 GiB |
| One UI (later) | Store: its own kernel, ramdisk, DTB | Card Google slot (after Android leaves it) | about 17.5-20 GiB |
| ChromiumOS / ChromeOS | Shared `linux.image/dtb/initrd` | Card p1 files (`/distros/chromeos/`) | 13.5 GiB per set |
| Windows 11 | SBSTORE `win11.uefi` | UFS WINESP + WINDOWS (Windows has no SD driver) | 12-14 GiB |
| Windows 10 | SBSTORE `win10.uefi` | Fixed `win10.vhdx` inside WINDOWS, pagefile outside it | 10-12 GiB + 1 GiB |
| postmarketOS | Shared `linux.*` | Card p1 `/distros/pmos.img` | 8 GiB file (2.1 GiB used) |
| Ubuntu Touch | Shared `linux.*` | Card p1 `/distros/ut.img` | 8 GiB [est] |
| Emergency OS | BOOT bundle `SBEMERG!` + copies in both stores | A small data record in the store | 2-4 MB |
| Utility OS | UFS store, card fallback | RAM | 6-12 MB |
| Engineer OS | UFS store, card fallback; Engineer Lite in BOOT | Settings and test history on the card | 10-16 MB |
| Installer / Starbite Recovery | RECOVERY + store `installer.*` | RAM | about 40-45 MB |

BOOT after the move: loader, menu v7 assets, Engineer Lite, the Emergency OS bundle and the iOS 26 payload. The Windows UEFI moves to SBSTORE. About 25-30 MB used of 55 MiB [est].

### 2.6 The master drive (a purchase: decision 1)

A USB drive of **256 GB or more** (512 GB is more comfortable), exFAT, plugged into the Mac.

| Folder | Holds | Size [est] |
|---|---|---|
| `backups/S9-1/<date>/` | S9 #1's irreplaceables + full partition images (USERDATA, WINDOWS, A16DATA, SYSTEM...) | about 50 GB |
| `backups/S9-2/<date>/` | Same for S9 #2 | about 5-40 GB |
| `card-p1/` | Everything on the 128 GB card's p1 before its re-layout | about 40-87 GB |
| `parked/` | iOS 16 root, Windows VHDX, parked Linux images | about 40 GB |
| `STARBITE/` | Installer media (§5.5) | about 50 GB compressed |
| `sources/` | macOS 26.5 parts, the Windows 10 image, GSI and recovery zips | about 30 GB |

It is the backup archive, the staging disk the Mac lacks, and (at first) the installer drive. The 64 GB stick stays as a second archive.

### 2.7 Couplings that break if the re-layout is done carelessly

| Coupling | What breaks | The fix (before layout B) |
|---|---|---|
| Old iOS payloads write the **last blocks of "USERDATA"** (the herolog), mount `ufsroot=USERDATA`, and find the store at "USERDATA + 16 GiB" (`store.c:9,136`, `starlte_overlay.c:4`, XNU patch 0206) | If Android's data took the name USERDATA, any old iOS payload would write into Android's data | Retire the name: USERDATA becomes a 256 MiB zero-filled dummy. The loader and XNU look up `SBSTORE` by name first. Only new payloads are ever booted |
| **Windows UFS driver write fence** is hard-coded (blocks 6613760..13518591, `gpt_backup = 15615995`; `NOTES.md` ~986-998) | A stale fence leaves Windows unable to write its own disk, or able to write Android's | Re-patch the constants (file offset 0x3ef0..0x3f18) to 2288384..9324287, re-sign, install into Windows 11's DriverStore and the Windows 10 VHDX, **in the same session** as the re-layout. Better long term: a fence by GPT GUID [unverified: needs the driver's source] |
| Windows' boot config names its disks by **GPT GUID** | Wrong GUIDs = Windows cannot find itself | Write the new GPT with the same partition GUIDs and disk GUID |
| NTFS "hidden sectors" field | Wrong value after a move | Fix the boot sector field after the restore |
| PIT vs GPT | Odin by name past SYSTEM would write the wrong place | Rule 1.2.2, unchanged |

---

## 3. Per category and per OS

Each OS has the same parts: what it is, today, feasibility, where it lives, how it boots, milestones ("done when"), effort, blockers, decisions.

### 3.1 Apple: iOS 26

**What it is.** iOS 26.6.2 (iPhone 11 identity) on our own xnu-12377 kernel. Metal runs on the Mali through StarbiteMetal → metal2vulkan → PanVK.

**Today: works (Beta).**
- 8 cores (the M3 cores' missing atomic instructions are emulated by traps, patches 0342-0344, plus 25 hot sites patched).
- Metal on the Mali, about 10 fps, up to 24 fps with logd and sharingd off, on an 828x1792 canvas scaled up.
- XNU sees 1792 MiB of RAM.

**Broken or missing** (`NOTES.md:1884-1937`)

| Item | State |
|---|---|
| Wallpaper | Black or red. PosterBoard never launches a poster extension |
| Control Center | Chrome and blur draw; modules are faint outlines (a glass shader computes wrongly) |
| Liquid Glass | Several shaders render wrongly or have no program |
| Widgets | Black boxes (a two-plane surface format) |
| Some icons | Plain (two CoreImage kernels fail) |
| About page | Says "iPadOS 26.6.2" |
| Speed | 10-24 fps; datamigrator runs at ~110 % CPU every boot |
| Metal per app | Each Metal app needs `sweetinject`; an arm64e plug-in build would remove that [unverified] |
| M3 traps | Still 3-4 M traps per 10 s; the ahead-of-time LDAPR→LDAR rewrite (~133 k sites) and full LL/SC stubs are not done |
| Hardware | No audio, Wi-Fi, Bluetooth, cellular, camera, GPS, sensors, video decode |
| Hygiene | logd off (no unified log); the root filled up once from logs |
| Packaging | The kernel ships stripped; BOOT now has room for the full one |

**Feasibility: moving the root to the card is likely.** The driver is built (commit 579ecb2): patch 0345 (PIO, read-only, MBR/EBR parsing, devfs `sdroot0`) and 0346 (`rd=sdroot0`). The probe payload is `out/xnu-sdprobe.xnu` with `tools/cmdline/ios26-sdprobe.txt`. The honest trade-off: the card is much slower than UFS until DMA (IDMAC) and UHS land.

**Where it lives.** Payload in BOOT. Root: UFS USERDATA today → card p5 IOS26.

**How it boots.** Catalog `ios26`, method `xnu`, files `["@boot"]`. The command line changes from the UFS root to `rd=sdroot0 sdroot=p5` (plus `sdrw=1 rootrw=1` once writes work).

**Milestones**

| # | Step | Done when | Effort |
|---|---|---|---|
| **SD1** | T1, the probe (`docs/XNU-MICROSD.md` §12.3): reads the MBR, p1, the store's CRCs, measures speed, then panics on purpose into TWRP with its log | The log shows the card, both partitions, matching CRCs and a speed figure | S |
| **SD2** | T2: a 64 MiB read-only HFSX test image at sector 249868288 as the root (§12.4) | XNU mounts it as root read-only and launchd runs the test job | S |
| SD2b | Host-side test of the MBR/EBR parser; `sbstore2.py --medium sd` reserves the card's tail | Tests pass; a large store build can no longer overwrite the test range | S |
| **SD3** | v1.1: `sdrw` writes (CMD24/25), the card log ring, `sdhs` 50 MHz | A write + read-back hash test on the scratch range passes on 3 boots in a row; read speed measured | M |
| SD4 | v2: IDMAC (DMA below 4 GiB), then UHS (1.8 V via LDO2) | Measured about 20 MB/s (IDMAC) / about 80 MB/s (UHS) [est] | M-L |
| **IO5** | Move the root: fresh pull of the live root to the master drive; `dd` to card p5; hash check; boot read-only, then read-write | iOS 26 runs daily from the card for 3 days; the root checks clean | S-M |
| IO6 | The fix list above, in parallel, one item at a time | Each item has its own check | L (open-ended) |

**Effort.** Card move M (SD1-SD3 + IO5, about 4-8 sessions). Fixes L.

**Blockers and risks**
- Speed regression on the card (PIO about 10-12 MB/s vs UFS).
- A write bug could corrupt the card. Mitigation: the scratch range first, the log ring, backups on the master drive.
- The backup `backup/ios26-root-20261102.img` may predate later edits; take a fresh pull before the move.
- Untested: compressing the root's files (`ditto --hfsCompression`) could roughly halve its size; it would help macOS too.

**Decision for you.** Accept a slower iOS 26 so Android gets its 24 GB, or wait for IDMAC first (decision 5).

### 3.2 Apple: macOS

**What it is.** A macOS userland on our own kernel. Our xnu-12377 is built from Apple's source with the **MacOSX** configuration (`xnu/series-12377/README.md:14,29`); today it runs iOS through a compatibility switch. A macOS userland is closer to what this kernel was built for.

**Version.** macOS 26.5 build 25F71 for the MacBook Neo (J700AP / Mac17,5, A18 Pro). It matches our kernel source (xnu-12377.121.6 = macOS 26.5) and the KDK we build with. macOS 27 needs an XNU that Apple has not published yet (the newest public tag is still xnu-12377.121.6).

**Today:** not started.

**Feasibility (honest)**

| Step | Likelihood |
|---|---|
| Kernel → launchd from a macOS root | Likely |
| A desktop that draws | Plausible, not certain |
| A smooth daily macOS | **Not realistic** on 1.8 GiB RAM, a card root and the Mali |

**Why not the Snapintosh way.** Snapintosh runs Apple's stock kernel under an EL2 hypervisor. On the S9, S-Boot hands over at EL1 (`shim/starlte/xnu_boot.S:22`), `hvc` is UNDEFINED, and ID-register reads trap (`NOTES.md:1089-1092`). Even with EL2, Apple's stock arm64e kernel would hit PAC instructions our cores do not have. Getting EL2 would need a hypervisor exploit on the VG6 bootloader (public ones target older chips and firmware, and rollback protection blocks downgrades). **Not a plan.**

**What we borrow from Snapintosh (MIT):** its list of device-tree properties iBoot normally fills in; its GPU-driver interface (AdrenoX2/AdrenoMetal headers) to check our IOGPU calls against; later its AIR shader front end. Its key lesson: **WindowServer refuses to start without a Metal device** ("No suitable Metal devices present after 30 seconds").

**The hard limits**

| Limit | Detail |
|---|---|
| **RAM** | XNU sees 1.5-1.8 GiB (one range below the GPU tables). macOS 26 VMs are given 8 GB or more. A "multi-region" kernel could add about 1.3 GiB from bank 0, but it is an invasive VM change: only after a desktop exists |
| **Instructions** | The macOS cache uses LDAPUR/STLUR (2,060 sites, neither core type has them) and possibly FlagM, FRINT, BF16, I8MM, SHA512/SHA3, SB, DC CVAP, which our census tool does not count yet. Each would crash, not slow down. **First host task: extend `tools/dsc-isa-census.py` and run it on the 26.5 cache** |
| **Root** | Must be on the card (UFS is full) and read-write: needs SD3 first |
| **No APFS, IOMedia or DiskArbitration** | Flatten System and Data into one HFSX volume; disable apfsd, diskmanagementd, storagekitd, kernelmanagerd. Finder will show no volumes |
| **WindowServer** | Needs Metal, the IOGPU calls macOS uses, and CoreDisplay's mode and EDID chain |
| **Display** | Portrait 1440x2960 panel. Run landscape at 1x 1480x720 with a 2x present (1.07 Mpx). 720 points is short: some windows will not fit |
| **Input** | macOS has no touchscreen support. The touch panel becomes an absolute pointer (tap = click, long-press = right click, two fingers = scroll); typing through the Accessibility Keyboard |
| **No USB host, Wi-Fi or audio in XNU** | Same as iOS 26 |
| **Mac disk** | The IPSW is 18 GiB. Fetch only the OS and SystemOS parts by ranged download (`tools/fetch-ios-parts.sh`), straight to the master drive |

**Where it lives.** Card p6 MACOS (32 GiB [est]). Payload `macos.xnu` in a store.

**How it boots.** Catalog `macos`, method `xnu`, files `["macos.xnu"]`, command line `rd=sdroot0 sdroot=p6 cpus=4` plus the iOS-style boot-args, **without** `unentitled_ios_sim_launch`. Hidden until MAC4.

**Milestones** (every boot is a hidden store entry; the daily iOS 26 boot is never touched)

| # | Step | Done when | Boots [est] |
|---|---|---|---|
| **MAC0** | Prerequisites: SD1-SD3 pass; card re-layout done; ISA census extended and run on the 26.5 cache; parts fetched to the master drive; `mkadt_starlte.py --j700` mode (Mac17,5 / J700AP, borrowing Snapintosh's placeholder list, keeping the S9's own CPU, memory and device nodes) | A read-write HFSX range on the card passes a hash check from XNU, and the census shows no unhandled instruction class | 10-25 |
| **MAC1** | `tools/build-macos-root.sh` (from `build-ios-root.sh`): merge the SystemOS cryptex, flatten, translate the cache (`a64e-rewrite.py --skip libcorecrypto`), ad-hoc sign. First launchd job: `dsh` or the a64e test | macOS launchd (PID 1) runs a job from the card and its output reaches the log | 5-15 |
| **MAC2** | Core daemons (notifyd, configd, cfprefsd, opendirectoryd, securityd, launchservicesd...), a macOS disable list, a pre-made user with autologin | The system is idle with no crash loops for 5 minutes | 10-30 |
| **MAC3** | **WindowServer (the gate).** `sweetinject` for WindowServer (or the arm64e plug-in), the IOGPU calls, the display mode chain | WindowServer composites a first frame on the Mali and loginwindow draws. **Go/no-go: if this stalls for long, macOS stops here** | 20-60 |
| **MAC4** | Desktop: Finder, Dock, menu bar; glass bugs as on iOS | Desktop stable for 10 minutes, at least 10 fps at 1x | 10-30 |
| **MAC5** | Input: pointer, gestures, Accessibility Keyboard, keys | You can open an app, type and quit with touch alone | 5-15 |
| MAC6 | Hardening: 8 cores, IDMAC/UHS, more RAM; macOS 27 when Apple publishes its XNU | Open-ended | — |

**Effort. XL**, about 80-150 boots (roughly 25-45 sessions) to MAC5 [est]. For comparison, iOS 26 went from kernel to "usable on the Mali" in about a day and 35 boots, but it sat on 10 days of earlier iOS stand-in work. macOS has no stand-ins yet.

**Blockers, ranked.** 1 RAM. 2 Card root (writes, layout, speed). 3 WindowServer's macOS-only needs (size unknown). 4 No APFS/IOMedia. 5 Mac disk space. 6 macOS 27 source not published. 7 No USB host/Wi-Fi/audio. 8 Display height. 9 Licence (personal experiment only).

**Free check (optional):** photograph EngineerMode's SoC page on each cluster once (it prints the EL2 field and 16 KiB page support). Not needed for this route.

**Decision for you.** Go ahead after SD3, with MAC3 as a go/no-go gate (decision 11).

### 3.3 Google: Pure Android 17

**What it is.** A Pixel-like Android 17 whose system is pure AOSP, not LineageOS.

**Today.** LineageOS 24 (Android 17) **boots** from the Starbite menu (you confirmed it 2026-11-03), on krazey90's Linux 4.9.337 kernel and vendor.

**The honest catch.** Only the **system** half can be pure AOSP. The kernel and vendor stay krazey90's: nobody else maintains S9 drivers or a 4.9 kernel that Android 17 accepts. That is exactly the split Google designed Treble for.

**Why it can work at all.** Android 16+ refuses kernels below 5.4 (its network BPF loader exits, and the phone reboot-loops). krazey90's kernel reports itself as "5.15.178" to exactly those loader processes and carries newer BPF code (`kernel/sys.c`, about lines 1271-1279). His AOSP patching is a single userspace patch, so compatibility lives in the kernel, and a Google-built system image should meet the same interfaces [est].

**Route A (first): Google's official Android 17 GSI on krazey90's vendor**

| Fact | Value |
|---|---|
| Image | `aosp_arm64-exp-CP2A.260605.012`: system.img 1.90 GB (3.72 GB with Google apps). Fits SYSTEM (4.75 GiB) |
| CPU | Built for plain ARMv8.0: fine on the M3 cores |
| 32-bit | Multilib: our 32-bit camera HAL can run |
| Interfaces | GSI LLNDK 202604 = vendor `ro.board.api_level` 202604: they match |
| Build | `user`, not debuggable: no adb root |
| Newer | QPR1 stable (Sept 2026 patch); QPR2/QPR3 betas (Oct 2026 patch) |

**Risks found before any boot**

| Risk | Detail | Mitigation |
|---|---|---|
| **SELinux** | The vendor policy has 290 `lineage` references and 36 Lineage attribute lines; Google's system policy has none. If init's policy compile fails, the phone reboot-loops | Check offline with `secilc` first (AN0); add missing declarations to the vendor copy |
| **Keymaster patch level** | BOOT's header says patch 2026-10; stable GSIs are 2026-06 / 2026-09 (older). [unverified] whether this blocks keys | Fresh data; or a QPR2/QPR3 beta GSI (≥ 2026-10); or our own build |
| **Officially unsupported** | GSIs need a phone launched with Android 9+; the S9 launched with 8.0. Works only because krazey's vendor is Treble-built | — |
| **New BPF needs** | Each new release's network module may need BPF features the 4.9 backport lacks | Stay on AOSP GSI modules (not Play-updated Google modules) |
| **What we lose** | VoLTE/VoWiFi (krazey's ImsStack lives in his system image), the stock camera app, SamsungDoze. Calls fall back to 2G where it exists | Route B brings VoLTE back |
| **Licence** | Testing only; no copying (except backup) or distribution | The installer never carries it; the phone downloads it |
| **Play Integrity** | Will most likely fail (unlocked bootloader): no Google Wallet tap-to-pay, some banking apps fail | — |

**Route B (later): our own AOSP 17 build ("Starbite Android")**
- A GSI-style product (aosp_arm64, userdebug) plus krazey's open-source ImsStack (VoLTE back), our Pixel-like defaults, optionally microG.
- **Needs an x86-64 Linux machine with at least 400 GB disk and 64 GB RAM.** The Mac cannot do it. Renting one is a decision (decision 7).
- Benefits: we may ship it in the installer, it is debuggable, it has VoLTE, and we control the patch level.
- AOSP source now drops twice a year (Q2 and Q4); build from `android-latest-release`.

**How Pixel-like can it get?** About 90 %: Material You, Quickstep gestures, Pixel-style Settings, plus Google's own apps from the Play Store. "Exactly a Pixel" needs proprietary Pixel parts (Pixel Launcher, Google Sans, At a Glance) taken from your own copy of a Pixel image, for personal use only.

**Google apps, three choices:** Google's GMS GSI (testing licence, no modification), MindTheGapps (widely used, but unlicensed redistribution), or microG (free software, needs Route B). After installing Google apps, register the device at google.com/android/uncertified.

**Where it lives**

| Phase | SYSTEM | VENDOR | /metadata | /data | Kernel |
|---|---|---|---|---|---|
| Test (AN1) | Card Google slot ← GSI | Card ← copy of krazey's vendor, fstab pointed at card nodes | Card | Card 12 GiB, fresh f2fs | Store `android.kernel` + `a17card.ramdisk` |
| Daily (AN3) | UFS SYSTEM ← GSI (LineageOS image kept as backup) | UFS VENDOR | UFS ODM | UFS A16DATA, fresh, 22.35-24 GiB | `android.*` unchanged |

**How it boots.** Test: hidden catalog entry `android-gsi`, method `android`, files `android.kernel`, `a17card.ramdisk`, `a17card.cmdline`. Daily: the existing `android` entry, version line updated.

**Milestones**

| # | Step | Done when | Effort |
|---|---|---|---|
| **AN0** | Download the GSI zip **on the phone** (pmOS, Wi-Fi), copy only the SELinux files, VINTF XMLs and build.prop to the Mac; run `secilc` on GSI + mapping + vendor policy | It compiles, or the list of missing types is known and patched | S |
| **AN1** | Card-slot boot (needs the card re-layout) | The setup wizard appears; keystore2 and vold logs are clean | M |
| **AN2** | Fixes: SELinux, RIL, brightness, camera through vendor overlays; Google-apps choice | Wi-Fi, BT, calls and SMS (2G), GPS, camera and fingerprint work | M |
| **AN3** | Promote to UFS: back up SYSTEM and A16DATA (hashes both sides), write the GSI, fresh A16DATA (same f2fs flags as now) | Daily-usable, and every other OS still boots | S-M |
| AN4 | Our own AOSP build on a rented x86 Linux machine | It replaces Google's binary, has VoLTE, and can ship in the installer | L |
| AN5 | Research: Android 17 on our mainline 7.1 kernel (Mesa, minigbm, drm_hwcomposer). No Samsung HALs, so no modem, audio or camera | Lab entry only | XL |

**Also:** fork krazey90's kernel, device and vendor repos now. He is the only maintainer below the system image.

**Effort.** M to a daily GSI; L for our own build.

**Decisions.** GSI binary now vs wait for our build (decision 6). Google apps choice (decision 7). Wiping LineageOS's data (decision 8).

### 3.4 Google: One UI (later maybe)

**Parked until Pure Android 17 is daily** (recommendation).
- Samsung stopped the S9 at One UI 2.5 (Android 10). Current One UI is 8.5 (Android 16) and 9 (Android 17).
- **Realistic: One UI 8.0 (Android 16) only**, from the community UN1CA 9810 port (S22 firmware donor, Note10 Lite vendor, DS-ACK 4.9.337 kernel). One UI 9 exists only as a work-in-progress for the S9+.
- **Its installer is destructive for Starbite.** It writes BOOT and deletes everything from SYSTEM to the end of the disk (iOS, Windows, Android data). **Never run it.** We extract its images on the phone instead.
- Lives in the card's Google slot (it needs about 17.5-20 GiB), with its own kernel, ramdisk and DTB as store entries.
- Will not work: Samsung Wallet/Pay, Secure Folder, Samsung Pass (Knox is tripped). Expect lag from the card.
- Milestones: **OU1** get a UN1CA starlte build and extract its images (L). **OU2** card slot and store entries; done when the One UI home screen appears (L).

### 3.5 Google: ChromeOS

**What it is.** Real ChromeOS. Step one is ChromiumOS built by Google from its own source; never FydeOS.

**Today:** not started.

**Key facts**
1. **Google publishes prebuilt ChromiumOS arm64 images** (`arm64-generic-public`, R156, built 2026-11-03, kernel 5.15, Chrome 156) including the exact sysroot to cross-build against. This meets "ChromiumOS from Google's source" without an x86 build machine.
2. arm64-generic is built for plain ARMv8.0 and uses software GL at first, so a first boot needs no GPU work. All official ARM Chromebook boards build for ARMv8.0 + crc + crypto, so they should run on our M3 cores [unverified: check crc32/aes/sha2 in `/proc/cpuinfo` under pmOS].
3. **minigbm** (Chrome's buffer allocator) has no backend for a display driver named `exynos`, which is our DECON driver's name (`tools/linux/port/src/gpu/drm/s9p/s9p-decon.c:64`). A one-line patch is required, or Chrome cannot allocate buffers.
4. **No KVM, ever** (EL1 only): no Android apps, no Linux apps (Crostini), no VMs.
5. **TPM:** ChromeOS needs one for login. We use a software TPM (swtpm + vTPM proxy), as Brunch does. **Not secure.**
6. **No ARM "Brunch" exists** (Brunch is x86 only). We would build the first one.
7. Panfrost drew the ChromiumOS UI on a Mali-G72 Chromebook board in 2021 with artifacts; our G72 renders Plasma today.

**Route C-A (first): Google's public ChromiumOS arm64-generic.** The loader boots our shared Linux kernel with `sb.root=/distros/chromeos/... sb.session=chromeos`. Our initrd (`tools/linux/init`) gets a "cros mode":
1. mount the card's exFAT;
2. loop-mount `rootA.img` (taken out of Google's image) read-only;
3. lay our overlay `sb-cros.img` on top (Mesa Panfrost, patched minigbm, swtpm, Chrome flags, power settings);
4. mount `state.img` as the stateful partition (patching how ChromeOS finds it, as Brunch does);
5. bind our modules image and switch to ChromeOS's init.

**Route C-B (second): real ChromeOS.** Same harness, but the root comes from an official recovery image **you** download from dl.google.com (proprietary; the installer never carries it).
- Donor: **geralt** (MT8188 tablet, kernel 6.1) first; **kukui** (same Mali-G72 family) as the alternative.
- Extra work: replace the board's Mali driver with our Mesa; model identity; inject the firmware node ChromeOS expects into the DTB (our loader already edits FDTs, `shim/fdt.c`); disable update_engine.
- Updates: fetch a new recovery zip, re-extract ROOT-A, keep the overlay and state.

**Route C-C (only if A and B hit a wall):** our own `overlay-starlte` build. Needs an x86-64 Ubuntu machine. XL.

**Not an option:** Googlebook OS (Aluminium). See §0.3.

**Graphics.** ChromeOS uses our mainline Panfrost and DECON driver, so the Linux GPU fixes (§3.9) help it 1:1: the 30 fps DECON flip cap, GPU page tables, power-domain cycling. The memory bus clock stays where S-Boot left it.

**Where it lives.** Card p1 `/distros/chromeos/`: `rootA.img` about 5 GB, `sb-cros.img` about 0.5 GB, `state.img` 8 GiB. A second set (13.5 GiB) for real ChromeOS later, in the Google slot space once Android has moved to UFS.

**How it boots.** Hidden catalog entry `chromeos`, method `linux`, files `linux.image`, `linux.dtb`, `linux.initrd`, command line `sb.root=/distros/chromeos/rootA.img sb.session=chromeos`. Category: the Google section.

**Milestones**

| # | Step | Done when | Effort |
|---|---|---|---|
| **CR0** | Build Mesa (Panfrost) and minigbm (+exynos) in the aarch64 VM against Google's Chrome sysroot; ChromeOS kernel options (vTPM proxy, overlayfs, dm, loop, ext4 encryption, SafeSetID, Landlock, zram, cgroup v2, squashfs/erofs) | Binaries built; `check-kernel.py` passes | S-M |
| **CR1** | Download the test image **on the phone** to the card; extract ROOT-A; create `state.img`; first boot with software GL; scale factor 3; idle suspend off | The ChromiumOS login screen on the panel; its sshd reachable from the Mac | M |
| **CR2** | Overlay Panfrost + patched minigbm | `chrome://gpu` shows Mali-G72 (Panfrost) with hardware compositing | M |
| **CR3** | swtpm login, Wi-Fi via shill, touch, keys, brightness, battery, manual rotation, menu entry, "reboot to Starbite entry X" | A full session survives a reboot | M-L |
| **CR4** | Real ChromeOS from the geralt recovery image | A Google account signs in; Chrome is GPU-accelerated | L |

**Unknown:** whether Google sign-in works on a ChromiumOS build without Google API keys [unverified]. Guest mode works.

**Effort.** M for ChromiumOS, L more for real ChromeOS.

**Decision.** Confirm C-A first, then C-B with geralt (decision 9).

### 3.6 Microsoft: Windows 11

**What it is.** Windows 11 23H2 (build 22631.7584) booted by our own UEFI: the woa bootshim, our own SEC/PEI stages, and the woa release's binary DXE volume (matched to build 22621.7582), with our PMIC clock module swapped in. Our own full DXE build never reached ExitBootServices, so **new firmware features are added as extra modules** into the release volume (`tools/woa/dxe-replace.py --add`).

**Today: works (Experimental)**

| Works | Missing |
|---|---|
| Desktop, OOBE done, autologon; touch; UFS (with our write fence); 3.4 GB RAM; correct clock; 4 A55 cores; restart/shutdown | USB, keys, battery, Wi-Fi/BT, audio, sleep, CPU clock and thermal control, the M3 cores, microSD, the GPU (draws in software on the firmware framebuffer) |

**All 8 cores is not only a Windows 10 thing.** 23H2 needs only ARMv8.0, like Windows 10 (only 24H2+ needs ARMv8.1). So the M3 work is proven first on today's Windows 11, which we know boots.

**The M3 fix we already have.** woa's firmware gave up on the M3 cores ("CPU_ON returns success but the core never arrives"). Our XNU patch 0304 solved exactly this on this phone: six PMU register writes before each M3 power-on (`xnu/patches/0304-starlte-m3-cores.patch`). Moving that into a UEFI module is the key new step.

**Also found:** woa's ACPI table has the CPU efficiency classes **backwards** (M3=0, A55=1, `Madt.aslc:49`). ACPI says lower is more efficient. Correct: A55=0, M3=1.

**Hazards [unverified, must test]**
1. If Windows turns on PAN or the newer atomics because the boot core (A55) has them, the first M3 hits an undefined instruction (XNU had to guard this per core).
2. If Windows tells apps the A55's features, an app thread that moves to an M3 crashes (the same problem iOS 26 has).
3. A55 core 1 and M3 core 0 share the same affinity ID 0x100.
4. Cache line sizes between the two core types are not measured yet.

**Milestones**

| # | Step | Done when | Effort |
|---|---|---|---|
| **W0** | Read-only: ACPI dump (`tools/woa/winpe/acpi-dump.ps1`), free space, driver KMDF versions, BCD export; from pmOS: cache line sizes per cluster, temperatures, clocks | Numbers recorded in `build/` | S |
| **W1** | Hygiene: block Windows Update by policy; cleanup; Compact OS; fixed 1 GiB pagefile | Updates blocked; free space measured | S |
| **W2** (WC1) | `Star2LteBigClusterDxe`: the 0304 PMU sequence, one CPU_ON each for M3 1-3 into a parking stub; ACPI rewritten to **7 cores** (A55 0-3 + M3 1-3, so no ID collision; classes fixed); BCD `numproc 7`. **Clocks and a temperature watchdog first** | Task Manager shows 7 CPUs; a 10-minute all-core load runs with no reset; temperatures logged; feature flags checked | M |
| WC2 | 8 cores: add M3 core 0, accepting the shared ID | Works or a clear failure | R (may not work) |
| WC3 | Boot on an M3 (A55 core 0 parked): Windows sees only ARMv8.0 features, which removes hazards 1-2. A 4-M3-only variant is also possible | Research | R |
| **W3** | USB host: Windows' own Synopsys DWC3 and xHCI drivers, if the firmware brings up the PHY, host role and VBUS (MAX77705) and describes the controller in ACPI | A USB keyboard, mouse and stick work | M |
| **W4** | Keys (small KMDF driver polling the same pins as `Star2LteKeysDxe`), battery (ACPI battery reading the MAX77705 fuel gauge over our I2C driver), temperature watchdog driver | Each shows in Windows | M |
| — | Wi-Fi/BT (BCM4361 on PCIe): no Windows driver exists. **Use USB dongles** | — | skip |

**Driver builds.** The Windows VM is gone and the Mac has no room. Route: a GitHub Actions Windows runner with the WDK NuGet packages; signing stays with our "Starbite Test Signing" certificate via `osslsigncode` on the Mac.

**Where it lives.** WINESP + WINDOWS on UFS. Boots via catalog `win11`, method `uefi`, files `windows.uefi` (moves from BOOT's bundle to SBSTORE).

**Effort.** M per item; 8 cores and the GPU are research.

### 3.7 Microsoft: Windows 10 ARM64

**Media.** Microsoft publishes no Windows 10 ARM64 ISO.
- **UUP dump** builds 19045 ARM64 images from Microsoft's own update files (newest seen 19045.7725, 2026-09-09). It warns ARM64 is unsupported.
- **Windows 10 IoT Enterprise LTSC 2021** (19044) is official, with ARM64 support until 2032-01-13, but licences come only through IoT distributors.
- Avoid unverified copies from archive.org.
- Windows 10 on Arm emulates x86 only, not x64: app compatibility is weaker than Windows 11's.

**The real blocker: the per-build firmware match.** The release DXE patches winload and ntoskrnl in memory for exactly 22621.7582. Windows 10 needs its own matched patch set. Its first hurdle is the timer frequency (S-Boot leaves CNTFRQ at 0; only EL3 can set it; Windows needs 26 MHz). Clues exist (woa's source finds the code cave "from PE section geometry ... 0x549E00 on 19041", and woa's research reached a 19041 kernel on the S9+).

**Process risk (honest).** Earlier sessions doing exactly this kind of Windows loader and kernel patch analysis were stopped by an automated safety check (NOTES.md, 2026-10-26). It may happen again. A legitimate shortcut you could choose to send yourself: ask ntdevlabs whether their 19041 research images or branch can be published.

**Building it without the Mac's disk** (all proven pieces)
1. In pmOS (Wi-Fi works), run UUP dump's Linux script; it writes `install.wim` to the card.
2. From TWRP or pmOS, copy the WIM to `C:\Starbite\Win10\` (ntfs-3g; needs a clean Windows shutdown).
3. **In Windows 11 on the phone:** create a **fixed** 10-12 GiB VHDX, apply the image with `/compact`, add the three S9 driver packages (the UFS driver must be boot-start), unattend file (skip OOBE, local account, 275 DPI).
4. BCD: `bcdedit /copy {current}` pointing at the VHDX. No `bcdboot` (the ESP keeps Windows 11's files), not in the visible list; selected once for tests.
5. Native VHDX rules: VHDX only, no hibernation, pagefile **outside** the VHDX, fixed size (a dynamic one expands to its maximum at boot), no BitLocker. **Native VHDX boot on woa's firmware is untested** [unverified]: test first on S9 #2 with the existing `starbite-win.vhdx`.

**Driver reuse.** The UFS driver targets 19041 already; touch needs KMDF 1.15 (fine). Still to check: the UFS driver's storport imports against 19041, and the I2C driver's KMDF version ≤ 1.31 [unverified].

**Two UEFI images, one shared base.** `windows.uefi` (22621 match) and `windows10.uefi` (19045 match) share the bootshim, SEC/PEI, the clock module and the big-cluster module. Each sets its own BCD default with `Star2LteBcdDefaultDxe` (designed in `docs/WINPE-ARM64-PLAN.md`).

**Milestones**

| # | Step | Done when | Effort |
|---|---|---|---|
| **W5** | Media + VHDX as above. A first ≤10 GiB VHDX fits today's WINDOWS free space (about 12 GiB), so this does **not** wait for layout B | The VHDX exists with drivers injected | M |
| **W6-T0** | Boot the VHDX once with today's unchanged `windows.uefi`; read the log markers (`=NTCFQ`, `=NTPATCH n=`, `=EBSOK`) in the next menu boot | We know how far the existing patcher gets (it may stop at the Samsung logo) | S |
| W6-T1 | `windows10.uefi` with a 19045-matched patch module: CNTFRQ first, then each patch class one by one | Windows 10 reaches its desktop | L-R |
| W7 | Per-entry UEFIs + BcdDefaultDxe + catalog entry `win10` | Both Windows versions boot from the menu | S-M |
| W8 | Windows part of the UFS re-layout (§2.7) | Both boot on layout B | M |

**Where and how.** Fixed `win10.vhdx` in WINDOWS. Catalog `win10`, method `uefi`, files `["win10.uefi"]`, root `ufs:WINDOWS \Starbite\Win10\win10.vhdx`.

**Effort. L-XL**, dominated by W6-T1.

**Decision.** UUP dump consumer build vs IoT LTSC (decision 12); whether to contact ntdevlabs (yours to send).

### 3.8 Microsoft: the GPU on Windows (shared by both)

- Arm ships Mali drivers for Linux and Android only. Adreno (Qualcomm) is the only working Windows-on-Arm GPU precedent.
- **wddm-mali-g52** (GPL-2.0, a Bifrost GPU like ours) drew a Direct3D 11 triangle through a real Windows display driver on 2026-10-02/03, with Mesa as the user-mode part. Its desktop on the GPU is not done yet. It is the realistic template.
- **Mesa removed its D3D10 user-mode driver in 26.3**; 26.2 is the last release with it. Pin 26.2 or carry it ourselves.
- Our G72 (Bifrost v6) differs from the G52 (v7) in GPU ID, quirks, MMU format and tiler.

| Stage | What | Effort |
|---|---|---|
| WG0 | Today: software drawing (WARP) | — |
| WG1 | More cores (WARP is multithreaded) + **hardware scaling in firmware**: report about 720x1480 and let the display engine scale to the panel, so WARP draws 4x fewer pixels [unverified: registers from Samsung's display driver] | M |
| WG2 | A display-only Windows driver for DECON, from Microsoft's KMDOD sample: page flips, vsync, brightness over DSI | M |
| WG3 | A KMDF Mali bring-up driver ported from our XNU StarbiteMali | M |
| WG4 | Fork or join wddm-mali-g52 for the G72: WDDM kernel driver + Mesa 26.2 + Panfrost → D3D11 → the desktop on the Mali. Temperature watchdog required first | L-R |
| WG5 | OpenGL/Vulkan on Windows; D3D12 | Not realistic |

### 3.9 Linux: postmarketOS

**What it is.** postmarketOS (Alpine-based, OpenRC, Phosh) on our mainline Linux 7.1 kernel (`7.1.0-starbite-23527494`, built from the SheroAbi S9+ port).

**Today: works (Beta).** The new pmOS image (`/distros/pmos.img`, 2143 MiB) booted 2026-11-03 with phoc, gnome-session, phosh and the on-screen keyboard. 8 cores, Wi-Fi, USB networking, Panfrost. The UI was measured at about 6 fps earlier (`docs/LINUX-REBUILD-PLAN.md` §1); fixes are designed. Not yet: Bluetooth, sound, sensors, camera, haptics, LED, brightness.

**The big find: a second mainline port on our exact SoC.** exynos-linux runs Linux 7.3 on the Exynos 9810 Galaxy Tab Active3 (GPL-2.0; status 2026-10-25) [claim]. It has:
- **a Panfrost fix for our exact GPU** (Mali-G72 r0p1 on the 9810): the AArch64 page-table quirk. Their test: the legacy tables fail in 5-6 of 10 runs; AArch64 tables pass 230 of 230. **Our 7.1 tree still uses the faulty legacy format** (checked);
- audio through A-Box without its firmware; the BCM477x sensor hub with our S9's compatible string; front camera capture through CSIS with stock libcamera; UART and Bluetooth; battery temperature; the root cause of the deep-idle hang; suspend to idle.

**"GPU stuck at 260 MHz" is the governor working as designed.** It sat there because the GPU was under 45 % busy. Fix correctness (page tables) first, then the 30 fps DECON flip cap, then measure. Raise the GPU floor only if the GPU is then the bottleneck.

**The one-big-flash idea (K2).** One kernel flash turns on every subsystem core and the built-in fixes. Later drivers are built as modules against the kept build tree and dropped into `modules-<release>.img` on the card: no more TWRP flashes for drivers. DT changes still change `linux.dtb`, which is why P1 matters.

**K2 contents:** Panfrost quirk + DECON `early_event` flip fix; 9810 UART + USI; the idle fix (off by default); BT core + BCM UART; sound core + Madera/CS47L92 (module); IIO (module); media/V4L2 (module); MAX77705 haptic and LED; 9810 ADC (module); optionally binderfs (for Waydroid); DT nodes for BT, haptic and LED.

**Milestones**

| # | Item | Done when | Effort |
|---|---|---|---|
| **P0** | Measure (no build): the rebuild plan's GPU triage, `upower -d`, backlight, Panfrost faults, the flip test | Baseline in `build/linux/runs/` | S |
| **P1** | Dev loop: Linux kernel/DTB/initrd in the **card store**, written from Linux; UFS copy kept as hidden fallback | A kernel or DT change needs no TWRP | S-M |
| **P2** | GPU: the page-table quirk (in K2); AFBC settings compared; flip fix confirmed; GPU floor only if needed | 0 GPU faults in 30 minutes; Phosh at 45 fps or more | S-M |
| **P3** | Bluetooth: BT core, 9810 UART patches, DT node with 3 GPIOs, BCM4361 firmware copied read-only from VENDOR, BCM4361 added to btbcm's table | hci0 up; scan and pair in Phosh; Wi-Fi unaffected | M |
| **P4** | Battery and charging: verify in UPower; port the Tab Active3 fixes and battery temperature with the S9's stock tables; keep stock current limits | Correct %, current, temperature; charging tested with you present | S-M |
| **P5** | Haptics + LED | Vibration on touch feedback; notification LED | S |
| **P6** | Brightness and blanking (DSI brightness command as a backlight device) | Slider works; the screen really powers down | M |
| **P7** | Audio: A-Box without firmware, CS47L93 codec (upstream Madera driver), **MAX98512 amp (no mainline driver: port Samsung's GPL one)**, machine driver + UCM | Speaker, earpiece, mic, headset in PipeWire. **No call audio.** Amp gain kept conservative (no speaker protection without A-Box firmware) | L |
| **P8** | Sensors: port the sensor hub driver; S9 patch blob from the stock kernel; mount matrix; iio-sensor-proxy | Auto-rotate; light and proximity | L |
| **P9** | Deep idle + suspend (fix behind a switch, default off) | C2 residency measured; suspend with power-key wake | M-L |
| **P10** | USB host / OTG (also needed by the Installer, §5) | A USB stick and keyboard work | M-L |
| **P11** | Camera: front S5K3H1 first, then rear S5K2L3 | The camera app shows the front, then the rear camera | L per sensor |
| **P12** | Modem | §3.11 | XL |

Not planned: fingerprint (TrustZone), GPS (closed protocol), iris. NFC low priority.

**Where and how.** Card p1 `/distros/pmos.img`. Catalog `linux-pmos`, method `linux`, `sb.root=/distros/pmos.img sb.session=phosh`.

**Risk.** The deep-idle fix had one hang on the Tab Active3 that the watchdog did not reset: you would need to hold Power + Vol Down.

### 3.10 Linux: Ubuntu Touch

**State of Ubuntu Touch (verified).** Release 24.04-2.0 (July 2026), then 2.1 and 2.2 (October). **Every 24.04 device is a Halium device** (Android drivers underneath). The PinePhone (UBports' mainline device) is stuck on 20.04. The current development branch is already on a 26.04 base with Lomiri 0.6.2, still on Mir 1.8.3, Qt 6.10 next to Qt 5.15, Mesa 26.0.

**Feasibility: yes in principle, but nobody ships it today.** The building blocks are maintained:
- the `ubuntu-touch-mainline` / `ubuntu-touch-core-mainline` packages in the 24.04-2.x repo (last built 2024-06, so expect some packaging decay);
- Mir 1's `mesa-kms` platform (rebuilt 2026-09-22);
- UBports' PDK virtual-machine images, which already run Ubuntu Touch without Halium.

**Lomiri on Panfrost should work.** Mir draws with GBM/EGL, the same Mesa path KWin and phoc already use on our phone. Risks: Mir must pick the DECON card (`exynos-bootfb` uses the same name); stale GPU reads until P2; speed (Lomiri's shell is CPU-heavy, plus the 30 fps flip cap); scale at 570 ppi (start at grid unit 24); and Ubuntu Touch services expect hardware we have not brought up (no dimming, rotation or sound until P6-P8).

**Not the old experiment.** The repo's `linux-ubuntu-lomiri` entry is the Ubuntu 26.04 desktop Lomiri, not Ubuntu Touch, and was never built. It stays parked.

**Milestones**

| # | Step | Done when | Effort |
|---|---|---|---|
| **UT1** | Lomiri smoke test: `mkroot-pmos.sh lomiri` → `/distros/pmos-lomiri.img` (pmOS's own Lomiri package, OpenRC like our image); DRM device set to the DECON card | Lomiri renders on the GPU (not software); touch works; fps logged | S-M |
| **UT2** | Ubuntu Touch root **built on the phone** (the Mac has no room): a noble minbase from the VM, an 8 GiB `ut.img`, the UBports repo pinned above Ubuntu, install `ubuntu-touch ubuntu-touch-core-mainline mir1-graphics-drivers-desktop ubuntu-touch-session-mir xwayland` (no `waydroid`: no binder). Alternative: stream the PDK image to the card and copy its root (3.6 GiB download: ask first) | apt resolves; Lomiri and Mir's KMS platform installed | M |
| **UT3** | Integration: user `phablet` uid 32011 vs our 10000 (add `sb.uid=` to the initrd); lightdm + lomiri-greeter; device YAML; mask the Android-style USB services (our initrd owns usb0); hide system-image updates; null sound sink | Greeter, PIN unlock, Terminal, Settings, Morph Browser, Wi-Fi from the indicator | M |
| UT4 | Fallback if Mir 1 fails on our DECON: Lomiri on Mir 2 needs qtmir rebuilt for Mir 2 (UBports' own unfinished work); realistic fallback is the desktop Lomiri (no phone apps or telephony) | — | L |
| UT5 | Hardware parity from §3.9. Telephony in Ubuntu Touch is oFono only | Per item | — |
| **UT6** | Menu: catalog `linux-ut` "Ubuntu Touch · 24.04-2.x · Lomiri · Linux 7.1", tag beta; follow 26.04 when UBports releases it | Visible in the menu | S |

**Where and how.** Card p1 `/distros/ut.img`. Method `linux`, `sb.root=/distros/ut.img sb.session=ut`. Check the Ubuntu/UBports trademark terms before sharing any image.

**Effort. M-L.**

### 3.11 Linux: the modem (shared with Emergency OS and Engineer OS)

The S9's modem is Samsung's Shannon 360, built into the SoC ("ss360ap"). **Nothing exists for it on mainline.** Only pieces:

| Piece | State |
|---|---|
| Samsung's GPL `modem_v1` driver (shared memory, mailbox) | Needs porting; only its headers are in our repo |
| Galaxy S7 modem booted with plain ioctls (2020, downstream kernel) | No calls |
| libsamsung-ipc (Replicant), `herolte` profile | Closest to ours |
| samsungipcd (AT commands to ModemManager) | Mobile data on a Galaxy J4 with a downstream kernel; SMS and calls untested; "early prototype" |
| Google's CPIF on the Pixel 7 (PCIe) | Different link from ours |
| an earlier port's plan for this exact phone | "No known boot of an integrated Shannon CP (8890 onward) without Samsung's downstream kernel" |

**Two extra walls.** Call audio needs the A-Box DSP's voice firmware (the firmware-free audio driver cannot carry calls). 4G voice needs an IMS client on the phone side, which we do not have [unverified]; without it, voice works only where 2G still runs (Finland requires 2G until the end of 2029).

**Path:** M1 shared-memory driver as a module (XL); M2 boot with a `starlte` libsamsung-ipc profile, firmware from a **RADIO image file from the backup**, NV data from an **EFS copy** only (L); M3 a virtual tty for ModemManager, or oFono for Ubuntu Touch (M); M4 data and SMS (L); M5 voice (XL).

**Safety:** refuse emergency numbers in the AT layer; turn off the emergency-call UI in test images; call only your own numbers.

**Cross-area conclusion.** Neither Linux nor the Starbite OSes will place calls soon. If anything needs calls, it is Android 17 with Samsung's own modem software, after checking that it works in our Android boot.

---

## 4. The three Starbite OSes

### 4.1 Hardware facts (corrections from the research)

| Assumed | Reality |
|---|---|
| PMIC "S2MPS18/S2MPS19" | **No S2MPS19** (that is the 9820). The S9 has S2MPS18 (main, behind ACPM), S2MPB02 (camera and flash), S2MPB03 (camera), MAX77705 (charger, fuel gauge, USB-C, haptic, RGB LED), and a wireless-charging receiver |
| Sensors on the CPU's buses | Accelerometer, gyro, magnetometer, barometer, light and proximity sit behind a **sensor hub inside the BCM4775 GNSS chip**; the CPU loads its firmware over SPI at every power-up |
| GNSS | The same BCM4775; the protocol is closed (Broadcom's `lhd`). **Blocked** |
| Fingerprint, iris | Both run in TrustZone. **Not usable** |
| Heart-rate | MAX86915 on I2C: **reachable** |
| USB | The DWC3 can do SuperSpeed, but our port runs **USB 2.0 high speed only** (about 30-40 MB/s in practice) |
| Extras | Notification RGB LED, 3.5 mm jack, SAR grip sensor, hall sensors, MST. **No IR blaster, no FM radio** |

**Where we get facts.** an earlier port (example-project/refport-starlte) is an iOS port to the same SM-G960F. On 2026-11-03 its docs report working speakers, the codec mic, the front camera as raw frames, the sensor hub (with firmware download), Bluetooth HCI, haptics and a USB serial gadget [claim]. Their kernel is APSL (not GPL-compatible), so **we read their documents for facts and never copy their code.**

**What we may port.** Our code is GPL-2.0-only, so we may port Samsung's kernel drivers, mainline Linux, U-Boot (GPL-2.0+), TinyUSB (MIT), lwIP (BSD), Heimdall (MIT), brcmfmac (ISC) and libsamsung-ipc (GPL-2.0+), with attribution. Firmware blobs (modem, Wi-Fi/BT, sensor hub patch, audio DSP) are read from your own phone at run time and never committed.

### 4.2 The Starbite core (libsb), shared by all three and the Installer

```
                          Starbite loader (BOOT)                  catalog entry, method "bare"
S-Boot ─► loader ─► menu v7 ─► star_entry ─► boot_bare() ─► x0 = FDT copy with /chosen/starbite,*
                                                    │
         ┌──────────────────────────────────────────┴───────────────────────┐
         ▼                        ▼                        ▼                ▼
  emergency.img            utility.img             engineer.img      (later: native installer)
  BOOT bundle +            UFS store,              UFS store,
  copies in both stores    card fallback           card fallback
         └──────────────── all linked against libsb (the Starbite core) ────────┘

 libsb:
   crt0    self-relocation, exception vectors, MMU + caches
   sys     FDT memory map, reserved memory, PSCI worker cores, timer, WFE/GIC idle
   bus     HSI2C, SPI, UART, GPIO, IO expander, PWM, ADC, ACPM
   power   S2MPS18 rails (policy table), MAX77705 (fuel gauge, charger, watchdog, OTG, haptic, LED), RTC
   store   UFS (read + fenced write), SD (read + write), stores, catalog, settings, GPT
   fs      ext4 read-only, exFAT (contiguous-file writer), FAT32, SHA-256, CRC32
   usb     DWC3 device (TinyUSB port), later xHCI host
   ui      sbui widgets, SB7 fonts, theme, dirty-rectangle compositor, on-screen keyboard, QR
   input   keys, multitouch, gestures, later USB HID
   log     Starbite log ring + card file, pmsg, crash QR
```

**Decisions built in**
- **Three images, not one.** A Utility driver bug must never stop the Emergency OS, which carries the smallest driver set (no PCIe, camera or USB host).
- **Self-relocating images.** The loader keeps its "no absolute relocations" rule (`shim/starlte/build.sh:43-45`). The OS images fix their own addresses at entry (about 50 lines), so ordinary C (TinyUSB, lwIP, tables of function pointers) just works.
- **Entry contract unchanged** (`boot_bare()`, `xnu_boot.c:877-898`) with two additions:
  - the loader passes an FDT copy with `/chosen/starbite,{key, reason, rtc-epoch, loader-version, settings-flags, battery-mv}` (reason: picked, boot-next, low battery, key combo, RTC alarm, charger plug);
  - a new file source `@bundle:<magic>` (Emergency OS: `@bundle:SBEMERG!|ufs:emergency.img|sd:emergency.img`). Today `boot_bare` only accepts store entries.
- **Memory:** free list = FDT banks minus every reserved node. Never touch the modem's 146 MB window (0xF0000000), its log area (0xF9200000) or A-Box's area (0xFB200000). Audio DMA stays below 4 GB.
- **Idle:** WFE with the timer event stream first (no interrupt controller needed); GIC interrupts with WFI once the modem is on.
- **Worker cores:** the other three A55s for drawing, hashing, imaging and stress (power-on proven in em.c). The M3s only for stress tests and benchmarks.

**The write fence (required before any write path ships)**
- Every block write goes through `sb_write(medium, lba, n)`. It checks the target against the live GPT by **name and range**, cross-checked with the stock PIT (`device/pit.txt`).
- Hard-coded never-touch list: EFS, CPEFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA0-2, DTB, DTBO, the GPT/PIT region (only Engineer OS Tier 2 "restore this phone's verified backup"), and the boot LUNs (**never, at all**).
- Utility OS gets **no UFS writes**: only the card, USB, and Starbite-owned store entries.
- This mirrors the fence already in our Windows UFS driver.

**UI toolkit (sbui)**
- Reuse menu v7's fonts, palette tokens and springs, and em.c's list, dialog and key-value widgets, as retained widgets (list, toggle, slider, tile grid, dialog, hold-to-confirm slider, keypad, chart, tab bar, status bar).
- **No full-screen glass in any of the three.** Glass is what makes menu v7 slow. At most one cached glass header.
- Speed order (shared with menu v7, §6): caches (done), dirty rectangles, bands on 4 A55 cores, half-resolution with the display engine's 2x upscale, later the 2D engine or the Mali.
- Every screen works with keys: Volume moves, Power selects, short Bixby goes back, long Bixby goes home.

**Test harness**
- A **fake modem** (a protocol simulator on the Mac and in the image, `modem=fake`): every call, SMS and SOS flow runs with no radio. It is the default for every test run.
- Host builds of the UI logic with frame dumps, like the menu v7 preview tools.
- A Starbite log on the card, plus a **crash QR** on screen (reason, address, log tail) for you to photograph.

**Effort of the core:** SB0 (core extraction, self-relocation, handoff, `@bundle:`, catalog entries, em.c ported) **M**; SB1 (buses + flash LED + MAX77705 haptic/LED/watchdog) **M**.

### 4.3 Emergency OS

**Purpose.** A phone OS for when everything else fails: dead OS, low battery, no network, panic. It starts in about 3 seconds, works by keys alone with gloves in the dark, never needs the card or a network, and is honest on screen about what works.

**Must (v1, no modem)**

| Feature | Detail |
|---|---|
| Fast start | A bundle in BOOT, already in RAM after S-Boot. The loader skips the intro when the reason is "emergency". Target: S-Boot's 2.6 s (`device/logs/sboot-log.txt:1536`) plus under 0.5 s |
| Quick launch | Hold **Vol Down** at loader start (needs one test that S-Boot ignores Vol Down alone [unverified]); "boot-next = emergency" from any OS; offered automatically below 5 % battery |
| Flashlight | 10 levels from the DT torch table, with the DT time limits |
| SOS strobe | Morse SOS on the flash LED, the screen and the RGB LED; every pattern stays at 3 flashes per second or fewer (photosensitivity limit). A separate beacon blink: one flash every 5 s |
| Screen light | Full white at maximum brightness |
| Vibration SOS | Pattern on the motor |
| Medical ID | Name, birth date, blood type, allergies, medication, conditions, donor, language, ICE contacts. Readable from the lock screen without a PIN; also as a QR code. Prepared on the Mac (`tools/emergency/medid.py` writes a store record) or edited on the phone |
| Offline info | Short first-aid cards (our own text or CC BY-SA, FI/EN), emergency numbers by country, "how to describe where you are" |
| Battery | %, live current, **time to empty from the fuel gauge**, hours left in this mode |
| Extreme saver | One A55 at its lowest clock; panel off after 20 s; radios never powered |
| Charging works | Kick the MAX77705 charger watchdog (one known write, as Linux does) |
| Clock, timer, alarm | PMIC RTC |
| Last known location | Any other OS with a fix (Android, pmOS) writes `{lat, lon, accuracy, time, source}` to the settings record; shown as coordinates and a QR code |
| Test-mode framework | Built before any modem code (below) |

**Should (v2, modem online)**
- Modem boot, SIM status, PIN entry, signal bars.
- Send and receive SMS.
- **Location SMS to ICE contacts only** (never to emergency numbers), for example "SOS from <name>. 60.1699 N 24.9384 E (±1.2 km, cell, 3 min) geo:60.1699,24.9384 bat 34 % 14:02".
- **SOS sequence:** hold Power 3 s or press it 5 times → 5 s countdown with Cancel → SMS to ICE contacts, strobe, siren, vibration, repeated every N minutes with fresh battery and location.
- "I'm OK" check-ins.
- Coarse location from cell IDs using an **offline OpenCelliD extract** on the card (CC BY-SA 4.0; about 105 MB compressed for the world, filtered by country on the Mac).
- **Siren** through both speakers, with a limited level and duty cycle (no speaker protection without the audio DSP firmware).
- **BLE SOS beacon** ("SOS <name>"), so rescuers with any BLE scanner can follow the signal strength.
- Sensor hub: compass, altitude, **fall / no-motion detection** that can start the SOS countdown.
- Pulse check, labelled "not a medical device".

**Later**
- Voice calls: signalling is moderate; call audio needs the DSP firmware and the modem's voice path [unverified]; 4G voice needs IMS, out of reach. Only 2G fallback where it exists.
- Emergency calls: see the safe test mode. Without GNSS we cannot send handset location the way Android and iOS do.
- GNSS (research). Offline maps (raster tiles we render from OSM data on the Mac; never bulk-download from tile.openstreetmap.org).
- **Beacon mode:** phone off, woken by the PMIC RTC alarm every N hours, sends a location SMS, powers off. Rough sum: about 5 mAh per check-in, so weeks at hourly check-ins on 2967 mAh [est]. Needs an RTC alarm write and a test of what S-Boot does on an RTC power-on.
- Wi-Fi SOS network name; NFC tag with the Medical ID; panel always-on mode.

**Safe test mode (non-negotiable, built first)**
1. **Every build is a test build** unless you explicitly ask for a live one. A permanent "TEST MODE" banner is drawn on every frame.
2. In test mode the dialer and SMS accept **only an allowlist**: your own test numbers. Everything else is refused before any modem message is built. (Blocking only known emergency numbers is not enough: the modem itself treats numbers on the network or SIM emergency list as emergency calls.)
3. Outside test mode a classifier still runs: a static world list (112, 911, 999, 000, 08, 110, 118, 119, 100-102, ...), plus the SIM's list, plus the network's list. Matches are only ever sent as the emergency call type, and never in any automated test.
4. The SOS flow in test mode sends to the test contact, prefixes "[TEST]" and plays the siren quietly.
5. The fake modem runs every flow with no radio, by default.
6. A live build shows: "Emergency calls through Starbite are untested. Use another phone if you can."
7. We never dial emergency numbers in any test.

**Power (to be measured, not promised).** We have no suspend-to-RAM (the firmware's sleep path goes through Samsung's power firmware). Screen-off standby will probably be **worse than Android's deep sleep**. The gains are short sessions with nothing in the background; power-off beacon mode is the real long-life answer. Engineer OS measures each mode with the fuel gauge, and Emergency OS shows those measured numbers.

**SAR note.** On Android, the grip sensor tells the modem when the phone is against the body so it lowers transmit power. Before our modem code transmits, we must report grip state or force the reduced-power setting. Research item.

**UI.** Black background, white text 72-120 px, six big tiles: **SOS · Light · Medical ID · Messages · Info · Battery**. Header: time, battery % and hours left, signal or "no modem", TEST MODE. Lock screen: clock + Medical ID; everything works without unlocking.

**Drivers**

| Driver | Exists | New |
|---|---|---|
| Keys, touch, framebuffer, brightness, PMIC, RTC read, fuel gauge/charger read, UFS read, store, settings | Yes | — |
| Generic HSI2C, GPIO, PWM | Partly (in em.c) | S-M |
| Flash LED (S2MPB02), MAX77705 haptic/LED/charger watchdog | — | S-M each |
| Store data record (A/B copies, CRC) for Medical ID and messages | — | S |
| RTC alarm write | — | S (later) |
| Siren (A-Box + MAX98512), BT HCI, sensor hub, heart rate | — | L, M, L, M (v2) |
| Modem: boot, IPC, SMS | — | XL (shared with Engineer OS) |
| ext4 read-only (NV data from EFS, read-only) | — | M |
| GIC + WFI | — | M (with the modem) |

**Boot path**

```toml
[[entry]]
key = "emergency"
name = "Emergency"
version = "Starbite Emergency OS 1 · test mode"
category = "starbite"
method = "bare"
tag = "stable"
files = ["@bundle:SBEMERG!|ufs:emergency.img|sd:emergency.img"]   # @bundle:<magic> is a new loader feature
```

**Milestones**

| # | Step | Done when | Effort |
|---|---|---|---|
| **E1** | Image in BOOT, quick launch, home screen, Medical ID, info, battery | Power-on to home screen under 3.5 s; every tile reachable by keys | M |
| **E2** | Torch, strobe, LED, vibration, charger watchdog kick | Charges for 1 h inside Emergency OS without the charger reporting "watchdog expired" | M |
| E3 | Power modes measured | A fuel-gauge table per mode | S |
| **E4** | Test-mode framework + fake modem | All flows pass on the fake modem | M |
| E5 | Modem online, SIM, SMS to your own test number (after the Engineer OS modem bench) | One SMS to your test number | XL |
| E6 | SOS sequence, location SMS, cell-ID location | The full test-mode SOS on the fake modem, then on the real one to your test number | L |
| E7 | Siren, BLE beacon, sensors | Each works | L |
| E8 | Research: beacon mode, voice, GNSS | — | XL |

**Effort.** v1 (E1-E4) **L**. v2 **XL**, dominated by the modem.

**Risks.** The modem may refuse our boot (EL3 signature check, state left by S-Boot). Samsung's message format may differ from libsamsung-ipc's older modems. Speaker damage from the siren. S-Boot on RTC wake. Unknown standby draw.

### 4.4 Utility OS

**Purpose.** A pocket toolbox with no Linux. Above all it turns the phone into USB devices. Plus measuring and radio tools within the law.

**The key new driver.** A DWC3 device-controller port for TinyUSB (TinyUSB's Synopsys port is DWC2 only). It unlocks mass storage, keyboard/mouse, serial, network, MIDI, webcam and sound card classes in one go.

**Must (v1)**

| Tool | Detail |
|---|---|
| USB drive | Several drives at once: a **RAM disk** (up to about 2.5 GB free RAM), the **microSD** (whole card or one partition, read-only or read-write), **UFS partitions read-only** (never-touch ones not offered) for backing up on the Mac, disk images from the card. About 30-35 MB/s from RAM, about 20 MB/s from the card |
| Keyboard and mouse | Keyboard, mouse, **touchscreen as trackpad**, media keys |
| Macro pad | A grid of your own buttons from `macros.toml` on the card |
| USB serial | The Starbite shell: logs and `sbctl` commands from the Mac |
| File manager | UFS read-only; card exFAT read-write |
| Hashing | CRC32 and SHA-256 of files, partitions, images |
| Disk imaging | Partition or card to an image and back (card or USB only; UFS restores are Engineer OS work), verified |
| Hex viewer/editor | Viewer for everything; editor for card files only |
| Calculator | Scientific + programmer mode (hex/bin/dec, bit fields) |
| Clock tools | Stopwatch, timer, alarm, world clock |
| Flashlight | Shared driver |
| USB charger tester | Live voltage, input current, charger type, limit, charge state |
| Screen tools | Ruler at the panel's real ppi, dead-pixel test, colour fields |

**Should (v2):** USB network (NCM/ECM, RNDIS for Windows) with a web file manager, tiny HTTP/TFTP servers and NTP; USB MIDI controller; **ISO and USB-CD emulation** (boot PCs from installer ISOs on the card: a rescue cart for computers); gamepad; BLE scanner and "find my tag"; NFC reader/writer for your own tags; sensor toolbox (compass, level, barometer, lux, vibration FFT); sound meter and tuner (uncalibrated, "approx. dB"); USB host tools once OTG works (descriptor viewer, USB-stick file manager and imager, USB keyboard, serial terminal for USB-serial adapters); QR generator; notes.

**Later:** Wi-Fi scanner; cell info from the modem; UVC webcam from the front camera (640x480 fits USB 2.0; 720p needs MJPEG); USB sound card; phone as a display (GUD for Linux, our own protocol plus a small Mac app); QR reader and magnifier; stroboscope/tachometer (with a warning); audio-jack oscilloscope; USB-PD info; phone as a Bluetooth keyboard; DisplayPort out (stretch); HID scripting **for machines you own or may test only**.

**Legal limits (shown in About and enforced in code where possible)**
- **Allowed:** showing broadcast metadata (Wi-Fi beacons, BLE adverts, cell info the modem already knows); normal active Wi-Fi/BT scans; your own NFC tags; transmitting only as the stock firmware does within its type approval.
- **Never built:** jamming of any kind (EU RED 2014/53/EU; US 47 U.S.C. §333); deauthentication floods; injection outside normal operation; changing the regulatory domain or transmit power; base-station emulation or IMSI catching; changing the modem or its RF data; **IMEI changes**; storing or decoding other people's communications; cloning cards you do not own; MST payment emulation. NFC payment-card reading is not offered.

**Drivers**

| Driver | Source | New |
|---|---|---|
| USB 2 PHY init | GPL: `phy-exynos9810-usbdrd.c` | S-M |
| Route D+/D- to the CPU (MAX77705) | GPL | S |
| DWC3 device + TinyUSB port | U-Boot DWC3 gadget (GPL) | **L** |
| USB classes | TinyUSB | M |
| CD-ROM drive emulation | — | S-M |
| exFAT read/write, FAT32 | — | L / M |
| SD write | — | M |
| lwIP + network glue | lwIP | L |
| USB host (OTG power, Type-C role, xHCI, storage, keyboard, serial adapters) | U-Boot xHCI (GPL-2.0+), Linux serial drivers | **L-XL** |

**UI.** Menu v7's visual language; launcher grid **USB · Radio · Files · Measure · Everyday**; status bar with USB role and functions, battery, time; on-screen keyboard; touch first, keys always.

**Boot path**

```toml
[[entry]]
key = "utility"
name = "Utility"
version = "Starbite Utility OS 1"
category = "starbite"
method = "bare"
tag = "beta"
files = ["ufs:utility.img|sd:utility.img"]
```

**Milestones**

| # | Step | Done when | Effort |
|---|---|---|---|
| **UO1** | DWC3 enumerates as a USB serial port on the Mac (Starbite shell). This gives **every OS a USB console** | The Mac sees the port and the shell answers | L |
| **UO2** | RAM-disk and card drives | The Mac mounts both; a hash check of a copied file matches | M |
| UO3 | Keyboard, mouse, macro pad | Types into the Mac | S-M |
| UO4 | File manager, hashing, imaging | A card image made and verified | M |
| UO5 | USB network + web file manager | A browser on the Mac lists the card | L |
| UO6 | MIDI, ISO boot | A PC boots an ISO from the phone | M |
| UO7 | USB host (shared with the Installer) | A USB stick reads | L-XL |
| UO8 | Radios and sensors as their drivers arrive | Per tool | — |
| UO9 | Camera, display, audio classes | Per class | L |

**Effort.** v1 **L**; v2 **L-XL**; later **XL**.

**Risks.** DWC3 without interrupts (polling works); USB stays at high speed; keep USB buffers in the low memory bank at first; OTG role switching on the MAX77705 is unproven anywhere in our tree.

### 4.5 Engineer OS

**Purpose.** The bench for every new driver: each driver appears here first as a test page, before Emergency or Utility use it. And the place where Starbite manages itself. Today's EngineerMode stays in the loader as **Engineer Lite** (hold Bixby), the rescue tool that works when the store is broken.

**Must (v1):** port em.c onto the core, then add pages.
- **All current pages** (device, SoC, memory map, reset reason, CPU, temperatures, clocks, GPU read, core bring-up, RAM test and bandwidth, panel, display test, brightness, touch, keys, RTC, timer, battery/charger, PMIC, UFS host, partitions, UFS health, speed, sector viewer, microSD, OS pages, store, start an OS, logs, retained records, command lines, registers, reboot menu, peek).
- **Dashboard:** live clusters and clocks, temperatures, fuel-gauge current graph, RAM, uptime.
- **Tests with history** on the card: CPU per core (A55 0-3, M3 0-3), full free-RAM test, UFS/SD read speed, display (colours, grey uniformity, banding, dead pixels, a burn-in *detection* check that never leaves bright static patterns on for long), multitouch paint with 10 fingers, keys, battery/charger, thermal.
- **Armed mode**, **PMIC rail control with guards**, **clock control with guards** (below).
- **Register peek / armed poke** with an allowlist of blocks.
- **Starbite management:** store browser and verifier (UFS and card), catalog editor (order, hidden, default, timeout, per-entry command line, boot-next), logs (pmsg, XNU ring, Starbite log, boot history), **GPT viewer** (both copies, CRCs, PIT cross-check).
- **Partition backup** to card or USB with SHA-256, read-back verify and a manifest.

**Should (v2):** stress tests with a thermal guard (all 8 cores, RAM bandwidth, GPU through the ported StarbiteMali job path; abort at 90 °C by default; battery temperature watched); GPU benchmark; UFS write test on a scratch store entry only; SD write test and fake-capacity check (armed, blank card only); haptic, LED, flash, speaker, earpiece, mic, headphone tests; sensor hub, heart-rate, grip, hall pages; NFC/BT/Wi-Fi chip tests; USB device and host tests; **battery health** (full capacity vs design, cycle count); **store writer**; **GPT editor** (the re-layout tool: entries 1-24 byte-identical, primary + backup GPT, a backup of the whole GPT/PIT head first); UP_PARAM splash manager; BOOT backup and restore; **make the installer drive**; **flash another S9**; UEFI Shell launcher.

**Later:** **modem bench** (where the modem bring-up happens first: boot stages, SIM, masked IMSI, signal, cells, **SMS to your own test number**; no calls; never emergency numbers); camera pages; Wi-Fi link pages; a fastboot server; `sbctl` remote control from the Mac over USB serial (the Mac runs every test while you only press buttons).

**Armed mode**
- Off by default. Arming needs battery ≥ 30 % for writes, then **hold Power 2 s and slide**.
- Disarms after 5 minutes idle, on leaving the page, and on reboot. A red banner and border on every frame.
- Every risky action shows a **change card** (target, old value or hash, new value or hash, backup location), then: automatic backup with read-back verify → the write → read-back verify → an audit-log line.

| Tier | Allows |
|---|---|
| **Tier 0** (default) | Read only |
| **Tier 1** (armed) | Peripheral rails within policy; CPU/GPU clock levels from the DT tables; register writes in allowlisted blocks; Starbite-owned store areas; OS partitions (SYSTEM, VENDOR, ODM, A16DATA, WINESP, WINDOWS, SBSTORE); the card; USB |
| **Tier 2** (second unlock: type the partition name) | BOOT, RECOVERY, UP_PARAM, DTB/DTBO, GPT re-layout. Never-touch partitions **only as "restore this phone's verified backup"**: the backup header must match this phone's UFS serial and the hash of what it replaces |
| **Never, at any tier** | Boot LUNs; MIF/INT; memory and CPU rails; fuses; secure registers; modem NV/RF data; IMEI |

**PMIC guard.** Every regulator marked always-on in the DT (memory, interconnect, CPU, DRAM and UFS rails) is **locked**: no write path is compiled. Peripheral rails (touch, card, NFC, codec, camera) are switchable in Tier 1, only inside the DT's min/max, with known-good codes, read-back after every write and an auto-revert timer. GPU rail: only the validated levels (618.75 mV, 950 mV). Charger: only toward safer values (lower current, lower float voltage for an "80 % limit"); never the fuel-gauge model, resets or the USB-C mailbox except the known opcodes (`build/em-research-battery.md` §3).

**Clock guard.** Only ACPM requests for domains 2 (A55), 3 (M3) and 4 (GPU), at frequencies from the DT tables. M3 above 1794 MHz only with the thermal guard running. **MIF and INT are not in the domain list at all**, so the request cannot be built. No direct PLL or divider writes.

**Boot path**

```toml
[[entry]]
key = "engineer"
name = "Engineer"
version = "Starbite Engineer OS 1"
category = "starbite"
method = "bare"
tag = "lab"
files = ["ufs:engineer.img|sd:engineer.img"]
```

UI: menu v7's language; tabs **Dashboard · Tests · Stress · Power · Clocks · Storage · Radio · Starbite · Installer · Logs · Tools**. Image about 10-16 MB.

**Milestones**

| # | Step | Done when | Effort |
|---|---|---|---|
| **EN1** | em.c pages on the core, launched from the catalog | Every current page works from the catalog entry | M |
| EN2 | Dashboard, tests with history, logs | History survives a reboot | M |
| **EN3** | Armed-mode framework with a **dry-run** mode (logs what it *would* write) | Dry runs match the intended targets | S-M |
| **EN4** | Write fence; store writer on the card and a scratch entry | A scratch entry written and verified; a never-touch target refused | M |
| EN5 | PMIC and clock guarded controls; stress tests | A 10-minute stress run stops itself at the temperature limit | M |
| EN6 | GPT viewer → backups → editor (editor proven on **S9 #2 first**) | S9 #2 re-laid out and read back | M-L |
| EN7 | New driver benches as each lands | Per driver | — |
| EN8 | Installer drive and flash-another-S9 (§5.7) | S9 #1 makes a drive that installs S9 #2 | M |

**Effort.** v1 **M-L**; v2 **L-XL**; modem bench **XL** (shared).

**Risks.** Every write path is new: prove the fence read-only first, then on a scratch entry. Stress tests heat the phone while charging. The GPT editor and the installer writes are the two most dangerous features.

---

## 5. The Starbite Installer

### 5.1 Your idea, made concrete

```
 S9 #1 (Engineer OS)                         S9 #2 (stock, in Download mode)
  "Make installer drive"  ──►  USB drive
  "Flash another S9"  ──USB OTG cable──►  Download mode: BOOT + RECOVERY only
        (or the Mac with Heimdall/Odin)            │ reboot
                                                   ▼
                                       Starbite Installer (in BOOT)
                                       "Waiting for Starbite installer"
                                       plug in the USB drive (OTG adapter) or insert the microSD
                                                   │ checks → backup → layout → write → verify
                                                   ▼
                                       Full Starbite with every OS → reboot into the menu
```

**One correction to the idea.** Samsung's Download mode cannot boot a ramdisk (there is no "fastboot boot" on Samsung). It can only write partitions. So "write the installer into BOOT" is the way, exactly as you said in your second version.

### 5.2 What a stock S9 must show in Download mode (read this first)

| Line | Needed | If not |
|---|---|---|
| PRODUCT NAME | SM-G960F (PIT tag LSI9810). S9 #2 is confirmed Exynos | Refuse. A Snapdragon S9 is a different platform |
| OEM LOCK | OFF (U) | Flashing fails ("Only official released binaries..."). Boot stock Android, finish setup online, enable Developer options, turn on OEM unlocking (the phone wipes). The toggle is hidden for about 7 days while KnoxGuard is "Prenormal" |
| FRP LOCK | OFF | "Custom binary blocked by FRP lock". Remove the Google account in stock Android |
| KG STATE | Checking (worked on S9 #1) | Prenormal: wait it out online. Active: blocked; the owner or seller must release it |
| Bootloader | G960FXXUHFVG6 | Flash `build/fw-vg6/BL_...VG6...tar.md5` first. Windows was flaky on VG4, and our splash image is built from VG6 |
| Knox warranty bit | Stated, not required | The first custom boot sets it permanently |

S9 #2 is wiped and was left after `print-pit --no-reboot`, which wedges the protocol: Download mode must be entered again (hold Vol Down + Power 7 s to leave, then Vol Down + Bixby + Power, then Vol Up).

### 5.3 Step 1: putting the installer into BOOT and RECOVERY

The same image goes into **both** partitions (both are before SYSTEM, so the rules allow it). S-Boot may boot RECOVERY once after a Download-mode flash, so either way the installer starts.

| From | How |
|---|---|
| **Mac** | Heimdall 2.2.2 (our proven build): `heimdall flash --BOOT starbite-installer.img --RECOVERY starbite-installer.img`. One flash per Download-mode session |
| **Windows PC** | Odin3 v3.14.4 + Samsung USB driver: a tar with `boot.img` and `recovery.img` in the AP slot. **Never** tick Re-Partition or Nand Erase. **Never** add a CSC (it schedules a data wipe) |
| **Linux PC** | Heimdall, Thor or Odin4 |
| **Another S9** (your idea) | S9 #1 runs the installer environment with USB host and a static arm64 Heimdall. Cable: OTG adapter on S9 #1, then USB-A to C into S9 #2. It reads S9 #2's PIT and **refuses unless it is the known stock PIT**, then writes only BOOT and RECOVERY from a hard-coded allowlist. BOOT is ≤ 55 MiB: seconds. Needs USB host (IN3) |

- **Raw images only.** Download mode mis-writes sparse images (`NOTES.md:31`).
- RECOVERY's stock recovery is overwritten by this step. That is fine: it is in Samsung's firmware tar.

### 5.4 Where the installer code runs

| Option | USB host | UFS / card writes | Work |
|---|---|---|---|
| **A. Loader + a bundled Linux 7.1 (chosen)** | Port agcarbajo's `s9p-usbc` + `s9p-usbhost` modules (USB host on the same SoC and MAX77705; keyboards, mice and storage claimed working) [claim] | Proven from Linux (`boot-flash-linux.sh`); card at 22.6 MB/s | M |
| B. Samsung's 4.9 kernel (TWRP's) + our initrd | Built in [unverified on our unit] | Yes | S-M; a binary kernel to maintain. **Fallback** |
| C. Bare metal in the loader | xHCI + mass storage + SCSI + exFAT + MAX77705 OTG ≈ 4-5k lines [est] | Needs bulk writes and mkfs equivalents | XL. **Parked**; may come later through libsb |

**The installer image** has today's BOOT layout: the kernel slot holds an installer build of the loader; the ramdisk holds a BOOT-resident Linux bundle (Image about 25.6 MB, DTB, initrd about 10-15 MB). About 40-45 MB in all [est]; BOOT is 55 MiB, RECOVERY 65 MiB.

The same file serves three roles:
1. the stock-phone installer (in BOOT and RECOVERY);
2. **Starbite Recovery** in RECOVERY (Vol Up + Bixby + Power, independent of BOOT). It replaces TWRP only once it has `adbd` and the parity our tools need;
3. the Engineer OS's helper runtime for making drives and flashing other phones.

The installer program itself (`sbinst`) is static and kernel-agnostic (musl, busybox), with a small fbdev + touch UI in our font style.

### 5.5 The USB drive: format and manifest

The drive is exFAT (FAT32 also works: no file exceeds 1 GiB), label `STARBITE`:

```
/STARBITE/manifest.toml          what to install (readable)
/STARBITE/manifest.sha256        hashes of every file below
/STARBITE/manifest.sig           optional Ed25519 signature (key made on S9 #1)
/STARBITE/boot/                  installer.img  starbite-boot.img  recovery.img  up_param-vg6.img
/STARBITE/ufs/                   system.sbi vendor.sbi odm.sbi sbstore.sbi winesp.sbi windows.sbi
/STARBITE/card/                  sbstore.sbi ios26.sbi macos.sbi  exfat/distros/pmos.img ...
/STARBITE/backups/<serial>/<date>/   EFS.img ... SHA256SUMS     (written by the installer)
/STARBITE/logs/<serial>-<date>.log   journal-<serial>.toml       (written by the installer)
```

**The `.sbi` image format** (new `tools/installer/sbimg.py`, in the style of `sbstore2.py`): a header, then a table of 4 MiB chunks. Each chunk is zero, raw or zstd, with the SHA-256 of its uncompressed data. A whole-image SHA-256 at the end. Split into files of at most 1 GiB. It is resumable and verifiable by read-back. It is not Android sparse.

**The manifest** (example; values are illustrative):

```toml
format = 1
made_by = "S9-1 Engineer OS 1"
layout = "B"
requires_model = "SM-G960F"
requires_bootloader = ["G960FXXUHFVG6"]
requires_ufs_blocks = 15616000
min_battery = 70
card_profile = "128"
disk_guid = "52444e41-494f-2044-4d4d-43204449534b"   # "ANDROID MMC DISK"; same on S9 #2? [unverified]

[[gpt]]            # entries 25+ only; entries 1-24 are copied from the target phone itself
index = 26
name = "WINESP"
guid = "b3babc53-..."      # fixed, so the Windows boot config works on every phone
first = 2288384
last = 2419455

[[item]]
target = "ufs:SYSTEM"
file = "ufs/system.sbi"
sha256 = "..."
action = "write"           # write | mkfs.f2fs | mkfs.exfat | zero | copy-files | verify
```

**Never on the drive:** anything unique to a phone (EFS, CPEFS, KEYSTORAGE, PERSISTENT, STEADY, PARAM, BOTA, serials) except in `backups/<serial>/`, and **no live user data**. Ship clean roots, not used systems: no iOS keys, no pmOS SSH keys or PIN, no Wi-Fi passwords. No Google GSI, ChromeOS recovery image or One UI (the phone downloads those and checks Google's published hashes).

**Size and time [est].** Today's set: about 36 GiB raw, 25 GiB compressed. Full lineup: about 76 GiB raw, 50 GiB compressed. A 128 GB drive is the minimum; the master drive works. Making it on S9 #1 over USB 2.0 takes about 45-60 minutes.

### 5.6 The install sequence

1. **Checks.** Model and bootloader from S-Boot's command line; UFS size 15,616,000 blocks (or allowed by the manifest); battery ≥ 70 % (one USB-C port: no charging during an OTG install); card present; EFS readable. Then the screen shows **"Waiting for Starbite installer"**. It polls for `/STARBITE/manifest.toml` on USB drives and the card, and also offers the Mac-network and second-phone modes.
2. **Show the plan.** What will be erased, what is kept, sizes and the time estimate. Vol Up = install; hold Power 3 s = confirm; Vol Down = backup only.
3. **Back up this phone first.** EFS, CPEFS, PARAM, UP_PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA0-2, MISC, DTB, DTBO, RADIO, OMR, BOOT, RECOVERY; the boot LUNs (read-only); the first 4 MiB of the disk (MBR, GPT, PIT) and the backup GPT. To `backups/<serial>/` with SHA256SUMS, **read back from the drive**. If the drive is read-only or full: stop. **No backup, no install.**
4. **GPT.** Entries 1-24, the protective MBR and the disk GUID are copied byte for byte **from this phone**; entries 25+ come from the manifest. Write both copies, read both back, re-read the partition table, check every partition node.
5. **UFS contents**, each checked chunk by chunk on read-back: SYSTEM, VENDOR, ODM; A16DATA (fresh `mkfs.f2fs`, then a dry-run fsck); SBSTORE; WINESP; WINDOWS. Every block device stays read-only except the one being written. Zero chunks are skipped after a UFS unmap only where the device reports that unmapped blocks read as zero; otherwise zeros are written.
6. **Card.** If it is not a Starbite card, an explicit "erase card" confirmation. Then MBR and logical partitions, the card store, the iOS 26 / macOS partitions, and exFAT p1 with its files.
7. **UP_PARAM (the splash)** only if this phone's UP_PARAM equals the known stock VG6 image; otherwise skip and log why.
8. **RECOVERY:** Starbite Recovery, read back.
9. **BOOT, last:** the final Starbite BOOT, read back. Until this step the phone reboots into the installer, so any failure can be resumed.
10. **Finish.** Settings record (default entry, card store on); log and journal to the drive; "Done, remove the drive"; USB power off; reboot into the Starbite menu.

**Time [est]:** about 40 GB to UFS at ~30 MB/s (about 25 minutes) plus 40-60 GB to the card at ~20 MB/s (35-50 minutes): **about 1-1.5 hours** for the full lineup; 45-60 minutes for today's set.

### 5.7 What the Engineer OS does

Engineer OS is bare metal. Until libsb has USB host, it **delegates** the heavy work: it sets boot-next to the hidden `installer` entry with a mode (the boot-next mailbox already exists), and the Linux environment does the work, then returns to the menu.

| Mode | Needs | What it does |
|---|---|---|
| **Make drive** (host write) | USB host (IN3) | Formats or reuses the drive (shows model and size, asks to confirm); packs `.sbi` images from this phone's own partitions and card; per-phone data only under `backups/<serial>/`; Android data excluded unless chosen; verified by reading back from the drive |
| **Be the drive** (gadget) | USB device mode (works in Linux today) | A read-only multi-drive gadget: drive 0 holds the manifest folder, the others the raw partitions. To a PC (which copies it to a stick) or straight into S9 #2's installer: phone-to-phone with no stick in between |
| **Backup only** | Card or USB | Irreplaceables + GPT to the drive or card |
| **Flash another S9** | USB host | Heimdall over OTG, allowlist BOOT + RECOVERY, PIT must equal stock |

Engineer OS's "Installer" tab is the UI for all four. This is also how S9 #1 later moves itself: it makes its own drive, then reinstalls from it.

### 5.8 Failure handling and backups

- **Journal:** every step and item is recorded on the drive. A re-run verifies items already done (verify-only pass) and continues.
- **BOOT is written last.** Until then a power loss just restarts the installer.
- **On any hash mismatch:** stop, show the item, offset, expected and read hashes; BOOT untouched.
- **Battery:** start at ≥ 70 %; pause cleanly between items below 25 %. Fallback for drive power: a powered hub or Y-cable [unverified on our unit].
- **EFS is never restored across serial numbers.** Restores of never-touch partitions only from the same phone's verified backup.
- **Writable set:** entries 25+, SYSTEM, VENDOR, ODM, RECOVERY and BOOT; the GPT and UP_PARAM only after backup and verify; the card.
- **Read-only always:** EFS, CPEFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA, DTB/DTBO, boot LUNs, PIT, MD5HDR.
- **Never:** Odin by name past SYSTEM; MIF/INT; any MAX77705 write other than agcarbajo's known OTG / D+D- sequence.
- **The way back to stock for S9 #2:** Samsung's full firmware with its PIT (Re-Partition) rewrites the GPT to stock. EFS is not in the firmware tar, so it survives. This is a deliberate exception to the GPT rule: **only on S9 #2, only after its backups are verified, and only if you say so.**

### 5.9 The second-S9 test plan

| # | Test | Writes | Done when |
|---|---|---|---|
| **IN0** | You read S9 #2's Download screen (photo). Fix OEM lock / FRP / KG if needed; VG6 bootloader if needed | Maybe the VG6 bootloader | State known; VG6 on it |
| IN0b | Heimdall TWRP into S9 #2's RECOVERY; full backup (as `backup/20261102-185306` plus BOTA, MISC, the LUNs, disk head and tail) to the Mac and the master drive | RECOVERY | S9 #2's irreplaceables in two places |
| **IN1** | Build installer v0: `sbimg.py`, manifest, `sbinst` (UI, checks, backup, GPT, writers, verify, journal), the loader's BOOT-resident Linux bundle. Media: microSD and Mac-over-USB-network | — (host) | Host tests pass |
| IN1-T1 | **Dry run** on S9 #2: flash the installer; it checks, waits, reads a prepared microSD, verifies the manifest; choose "backup only" | BOOT, RECOVERY | Its backups match IN0b's hashes |
| IN1-T2 | **Minimal install**, layout B: GPT + Starbite BOOT + pmOS (card) + Android 17 | UFS user area, card | The menu appears; pmOS and Android boot |
| IN1-T3 | Add Windows 11 and iOS 26 (card root) | More | All installed OSes boot |
| IN1-T4 | **Failure tests:** force power-off mid-write (hold the keys) → re-run resumes; a corrupted chunk on the media → refused with a hash error; a wrong-model manifest → refused; a full drive → stops before writing | — | Each case behaves as designed |
| IN1-T5 | (Optional, your call) back to stock with Samsung's firmware + PIT, then reinstall | Full stock flash | The install is repeatable from stock |
| **IN3** | USB host: port `s9p-usbc` + `s9p-usbhost`; test an OTG stick on S9 #1 in Linux (you plug in the stick) | — | A stick mounts and reads at ≥ 25 MB/s |
| IN3-T1 | Installer v1 on S9 #2 from a USB drive | As IN1-T2 | Same result from a USB drive |
| **IN4** | S9 #1 Engineer OS makes the drive and flashes S9 #2 over OTG | S9 #2 BOOT/RECOVERY + install | S9 #2 goes from stock to full Starbite with no computer involved |

### 5.10 Prerequisites, effort, risks

**Prerequisites:** the GPT tool; Linux UFS and card writers (exist); the `.sbi` packer; the manifest and SHA-256/Ed25519 (TweetNaCl-size); for v1 USB host and Heimdall built static for arm64.

**Effort.** v0 (microSD and Mac-network media) **L**. v1 (USB drive + flash-another-S9) **M-L** more.

**Risks, ranked**
1. The USERDATA-name hazard (§2.7): fixed by the dummy partition and new payloads only.
2. The Windows fence and backup-GPT constants are hard-coded: a different layout or UFS size silently breaks Windows writes.
3. USB host is unproven on our unit; MAX77705 OTG writes are a new class of PMIC write.
4. Odin user errors (CSC, Nand Erase, Re-Partition).
5. KnoxGuard / OEM lock on S9 #2.
6. One USB-C port: no Mac logs and no charging during an OTG install.
7. Install time (about an hour).
8. Legal: personal use only; never publish media with Apple, Microsoft, Google or Samsung binaries.

---

## 6. Menu v7 speed

**What it is.** Our iOS-27-style Liquid Glass boot menu, drawn in software on one A55 core.

**Status today**
- It runs on the phone (device diagnostics commit 6600352; edge-stripe fix e9a6acb) and is **too slow**.
- Safety nets exist: if the first bake takes over 400 ms, menu v6 runs instead (`starmenu7=0` forces v6); if a full frame takes over 300 ms, animations snap and the pick is returned at once (`docs/MENU7-BUILD.md` §5).
- Speed work so far (host timings in the VM, ms per frame, `docs/MENU7-BUILD.md` §6):

| Frame class | Before | Round 1 | Round 2 + fixes | A55 estimate [est] |
|---|---|---|---|---|
| list scroll | 81 | 24 | 13.2 | 159-238 ms |
| section expand | 57 | 18.7 | 18.9 | about 227 ms |
| overlay moving | 65 | 22.5 | 19.3 | 231-347 ms |
| launch card | 101 | 38.1 | 23.5 | about 282 ms |
| intro (glass) | 99 | 32.8 | 34.5 | about 414 ms |
| lens | 14 | 5.5 | 4.8 | about 57 ms |

- **Round 3 part 1** (commit aa344ff, merged 137ab5f): glass shaded at half resolution at every frame, cached shape preparation. **Built, not flashed.** It ships with the One mark splash pair listed in `docs/MENU7-BUILD.md` §1 (loader b47e5c6f..., ramdisk d3fcca0b..., UP_PARAM 5bb50d61..., test image `out/xnu-boot-ios26-v7test.img` 4d36a1bc...).
- **The A55 factor is not measured.** The ×12-×18 column is a guess. At 30 fps a frame has 33 ms; **none of the moving classes reach 30 fps on the A55 yet.**

**Remaining steps**

| # | Step | Done when | Effort |
|---|---|---|---|
| **MV1** | Flash the pending pair (one session). Run one of `scen.sh`'s key scripts on the phone. Read log slot 116 (splash capture), slots 128-138 (per-class frames / worst / total ms) and 140-156 (bench and phases) | A measured A55 factor per class replaces the guessed column | S |
| MV2 | **CPU boost**: raise the A55 cluster from S-Boot's 1050 MHz to 1690 MHz through ACPM domain 2 (the same request XNU sends; ACPM sets the voltage). Behind `M7_CPU_BOOST`; **needs your go-ahead** (`docs/MENU7-PORT.md` §2.4). Expected about 1.6x on CPU-bound classes [est] | Frame times re-measured; no instability across 10 boots | S |
| MV3 | Round 3 part 2: the classes still expensive: the intro's wallpaper (4.7 ms build + ~10 ms upsample a frame), the opening section's own glass during expand, the overlay's motion rows, the growing launch card, corners per pixel, the soft blur at each freeze (19 ms), the curtain's first frame | Host mean under 2.8 ms VM for list and expand (the 30 fps line at ×12) | M |
| MV4 | Hardware 2x upscale: draw at 720x1480 and let the display engine (DPP) scale to the panel. an earlier port does DPP scaling, so it is possible [claim]; registers from Samsung's display driver [unverified] | 4x fewer pixels per frame; measured | M |
| MV5 | Mali-drawn menu through our own GPU job path (the code that drew XNU's first triangle) | Glass on the GPU | L-XL |

**Not an option for the menu:** drawing with the other cores. The menu hands the phone to another OS, and a core cannot be switched off cleanly here (em.c "Parked core"; `docs/MENU7-PORT.md` §2.4). Multi-core drawing is for the Starbite OSes only.

**Done when:** list scroll and section expand run at 30 fps or better **measured on the phone** (slots 128+), and v7 never falls back to v6 on a normal boot.

---

## 7. THE ORDER OF WORK

**Rules for the order**
- **At most 3 active tracks** at once. Host-only work (no phone time) may run in the background.
- **At most 1 track per phone writes storage outside our own stores** at any time (an OS writing its own disk, such as Windows creating its VHDX, is normal use).
- **S9 #2 takes the risky storage and installer work. S9 #1 takes OS bring-up.**
- Each phase ends when its "done when" holds. No dates.

### 7.1 The critical path

```
 PHASE 0              PHASE 1                        PHASE 2                         PHASE 3
 master drive ───────────────────────────────┐
 + fresh backups                             ▼
                                     CL1 full card re-layout ──► IO5 iOS 26 root on card ─┐
 SD1 probe ─► SD2 RO root ─► SD3 writes ─────┘                    (speed gate: SD4 DMA)  │
                                                                                          ├─► UL2 UFS layout B ─┬─► AN3 Android data 22-24 GiB
 UL1 prep: loader + XNU find SBSTORE by name, dummy USERDATA, Windows fence re-patch ─────┤                     └─► W5/W7 room for Windows 10
 IN1 installer v0 ─► IN1-T2 S9 #2 on layout B (proves the GPT tool and writers) ──────────┘
```

**Windows 10's first test is not on the critical path.** A ≤10 GiB VHDX fits today's WINDOWS free space (W5, W6-T0). Only its comfortable final size waits for layout B.

### 7.2 Phase 0: facts and safety (now)

Goal: know S9 #2's state, have every irreplaceable in two places including the master drive, record baselines.

| Track | Items | Phone |
|---|---|---|
| A. Safety | Buy the master drive (decision 1). Mirror `backup/20261102-185306`, the USB stick archive, the 128 GB card's p1 contents, the 64 GB card (its iOS 16 root has no other copy) and a **fresh pull of the live iOS 26 root** onto it, all hash-checked | S9 #1 (TWRP pulls) |
| B. S9 #2 | **IN0** read the Download screen; fix OEM/FRP/KG; VG6 if needed. **IN0b** full backup to the Mac and the master drive | S9 #2 |
| C. Read-only + host | **W0** Windows dumps, **W1** block Windows Update; **P0** pmOS baseline (GPU triage, CPU features for ChromeOS, cache line sizes, temperatures); **AN0** SELinux offline check (GSI downloaded on the phone in pmOS); **MV1** flash the pending menu pair and read its timings | S9 #1 |

**Done when:** S9 #2's state is known and its irreplaceables are in two places; S9 #1's are on the master drive too; W0, P0, AN0 and MV1 results are written down.

### 7.3 Phase 1: foundations

Goal: XNU can write the card; the Starbite core exists; Linux's GPU is correct.

| Track | Items | Effort |
|---|---|---|
| **A. Critical path** | **SD1** probe → **SD2** read-only root → SD2b parser test + tail reservation → **SD3** writes + log ring + 50 MHz | M |
| B. Starbite core | **SB0** libsb, self-relocation, FDT handoff, `@bundle:` source; catalog: a Google section, a Starbite section, built-in row renamed "Engineer OS", hide the parked entries (`linux-shell`, `linux-ubuntu*`, `linux-arch`, `linux-kali`, `linux-olddtb`), re-point the hidden `linux` alias to pmOS, add a hidden `installer` entry, raise CAT_MAX 32 → 64 (hide entries; `sbstore2.py` refuses to drop live keys) → **EN1** Engineer OS on the core | M |
| C. Linux | **K2** one kernel flash → **P1** card-store dev loop → **P2** GPU measured | M |
| (background, host) | **MV3** menu round 3 part 2; MV2 if you agree | M |

**Done when:** XNU writes and reads back a scratch card range with matching hashes on 3 boots; Engineer OS boots from its catalog entry; Phosh renders on the GPU with 0 faults in 30 minutes.

### 7.4 Phase 2: the card library

Goal: the card holds iOS 26's root and the Android test slot.

| Track | Items | Effort |
|---|---|---|
| **A. Critical path** | **CL1** full card re-layout on S9 #1 (MBR/EBR built on the host; rehearsed first on the spare 64 GB card, after its contents are on the master drive (its iOS 16 root exists nowhere else); then written from TWRP and read back; card store rebuilt at 8 GiB; p1 recreated and OS files copied back from the master drive) → **IO5** iOS 26 root to p5 (read-only, then read-write) → speed gate: **SD4** IDMAC if reads are too slow | M |
| B. Android | **AN1** GSI card-slot boot → **AN2** fixes | M |
| C. Starbite | **SB1** buses + flash LED + MAX77705 → **E1** Emergency OS in BOOT → **E2** light, strobe, vibration, charger watchdog | M |

**Done when:** iOS 26 runs daily from the card for 3 days; the GSI reaches its setup wizard from the card; Emergency OS starts in under 3.5 s.

### 7.5 Phase 3: installer v0 and the UFS re-layout

Goal: both phones on layout B. Android gets its data partition; Windows gets its room.

| Track | Items | Effort |
|---|---|---|
| **A. Critical path** | **UL1** prep: loader and XNU look up SBSTORE by name; dummy USERDATA; Windows fence re-patched and re-signed; old payloads retired → **UL2** S9 #1 to layout B (installer in Mac-network mode, payloads streamed from the master drive): WINESP/WINDOWS cloned and restored with the same GUIDs, fence installed in the same session, fresh A16DATA → **AN3** GSI promoted to UFS with the fresh data (if you chose the GSI) | M-L |
| B. Installer | **EN3-EN4** armed dry-run + write fence → **IN1** installer v0 → **IN1-T1..T4** on S9 #2 (S9 #2 reaches layout B first, before S9 #1) | L |
| C. Windows | **W2** 7 cores + clocks and temperature watchdog on 23H2; **W5** Windows 10 media + VHDX; **W6-T0** first test | M |

**Order inside the phase:** S9 #2 reaches layout B (IN1-T2) **before** UL2 runs on S9 #1. The Windows track pauses on S9 #1 while UL2 clones and restores WINDOWS (W5's VHDX is simply carried along).

**Done when:** both phones are on layout B; Android data is 22.35-24 GiB; Windows 11, iOS 26 (card), Android and pmOS boot on both; the S9 #2 failure tests pass.

### 7.6 Phase 4: fill the lineup

Goal: every lineup entry has at least a first boot. Take **2-3 at a time, in this order** (most value per session first):

1. **ChromeOS** CR0 → CR1 → CR2.
2. **Utility OS** UO1 → UO2 (also gives every OS a USB console).
3. **Installer v1** IN3 USB host → IN3-T1 → **IN4** phone to phone (with EN8).
4. **Ubuntu Touch** UT1 → UT3, UT6.
5. **Windows** W3 USB host, W4 keys/battery/thermal, W7 menu entries.
6. **Emergency OS** E3 → E4 (power table, test-mode framework, fake modem).
7. **postmarketOS** P3 → P6 (BT, battery, haptics/LED, brightness).
8. **macOS** MAC0 host work (census, J700 device tree, `build-macos-root.sh`, parts on the master drive) → MAC1 launchd.
9. **Engineer OS** EN2, EN5, EN6 (stress, guarded controls, GPT editor proven on S9 #2).

**Done when:** each of the 11 lineup OSes has booted at least once from the menu (macOS: to launchd; Windows 10: at least the T0 result), and S9 #2 has been installed from a USB drive made by S9 #1.

### 7.7 Phase 5: the long ones, one at a time

- **macOS** MAC2 → MAC5, with the go/no-go at MAC3.
- **Windows 10** W6-T1 firmware match (if T0 failed). **Windows GPU** WG1 → WG4.
- **postmarketOS** P7 audio, P8 sensors, P9 idle/suspend, P10 USB host, P11 camera.
- **Modem** (shared): Engineer OS modem bench (modem online, SIM, SMS to your own number) → **E5-E6** Emergency OS v2 → Linux M1-M4.
- **Real ChromeOS** CR4. **Our own Android build** AN4 (rented machine). **One UI** OU1-OU2.
- **iOS 26** fix list IO6, continuously.
- Menu v7 MV4, then MV5.

### 7.8 Effort at a glance

| Phase | Rough size |
|---|---|
| 0 | S-M (mostly host work and a few button sessions) |
| 1 | M per track |
| 2 | M per track |
| 3 | L (the installer is the big one) |
| 4 | L-XL overall, M per item |
| 5 | XL, open-ended |

---

## 8. Decisions for you

1. **Buy a master USB drive** (256 GB or more; 512 GB is more comfortable)? Recommended: yes, before anything else.
2. **Card layout:** approve the full re-layout and the map in §2.4 (exFAT 36 GiB, store 8, iOS 20, macOS 32, Google slot 17.4)?
3. **Android data size:** 24 GiB, **24 GB decimal = 22.35 GiB (recommended)**, or 20 GiB? This sets Windows' room (§2.3).
4. **Windows 10:** a fixed VHDX inside WINDOWS (recommended), or its own partition?
5. **iOS 26 on the card:** accept a slower iOS until DMA lands, or wait for IDMAC first?
6. **Android system:** Google's GSI binary now (testing-only licence, no VoLTE), or wait for our own AOSP build?
7. **Google apps** (none, microG, MindTheGapps, GMS GSI), and **may we rent** an x86 Linux machine (≥400 GB disk, ≥64 GB RAM) for our own build?
8. **May LineageOS's Android data be wiped** when the GSI moves to UFS?
9. **ChromeOS order:** Google's public ChromiumOS first, then real ChromeOS from the geralt image (or kukui)?
10. **One UI:** parked until Android 17 is daily (recommended)?
11. **macOS:** go ahead after SD3, with WindowServer (MAC3) as the go/no-go gate?
12. **Windows 10 media:** UUP dump 19045 or IoT Enterprise LTSC 2021 (needs a distributor licence)? And will you ask ntdevlabs about their 19041 research?
13. **Emergency OS:** a bundle in BOOT (recommended) and Vol Down held as its quick key (needs one test)? Your **test phone numbers** and **ICE contacts**? A live (non-test) emergency-call build: **never** is the default.
14. **Installer:** microSD version first, then USB (recommended)? Linux-based (recommended)?
15. **S9 #2:** may it be wiped and re-laid out as the test phone? Use the 64 GB card in it? May it go back to stock with Samsung's PIT once (IN1-T5)?
16. **Menu v7:** may the loader raise the A55 clock to 1690 MHz (MV2)?
17. **TWRP:** replaced by Starbite Recovery only after it has adb parity (recommended)?

---

## 9. Sources

### 9.1 Repo files used

- `NOTES.md` (hardware facts 118-140; UFS fence ~986-998; EngineerMode 1060-1110; DVFS and the MIF/INT freeze 1655-1682; iOS 26 1823-1937; phase 0 1952-1973; Darwin, UEFI, splash 2029-2077)
- `docs/STARBITE-2-PLAN.md`, `docs/XNU-MICROSD.md`, `docs/ANDROID-PLAN.md`, `docs/LINUX-REBUILD-PLAN.md`, `docs/WIN11-24H2-A55.md`, `docs/WINPE-ARM64-PLAN.md`, `docs/UEFI-SHELL.md`, `docs/MENU7-BUILD.md`, `docs/MENU7-PORT.md`, `docs/IOS14.md`
- `shim/starlte/` (`em.c`, `hw.h`, `sd.c`, `store.c`, `store.h`, `ufs.h`, `catalog.h`, `catalog.c`, `menu7.c`, `xnu_boot.c`, `xnu_boot.S`, `build.sh`), `shim/fdt.c`
- `tools/store/catalog.toml`, `tools/store/sbstore2.py`, `tools/mkpayload-ios26.sh`, `tools/cmdline/ios26-sdprobe.txt`, `tools/a64e/a64e-rewrite.py`, `tools/a64e/lse-hotpatch.py`, `tools/woa/gpt-split.sh`, `tools/woa/fv-splice.py`, `tools/woa/dxe-replace.py`, `tools/woa/winpe/acpi-dump.ps1`, `tools/linux/init`, `tools/linux/patch-mainline.py`, `tools/linux/distros/{mkroot-pmos.sh, mkroot-ubuntu.sh, sb-integrate.sh}`, `tools/linux/port/src/` (DECON, USB PHY, MAX77705, PCIe)
- `xnu/series-12377/` (README, 0345, 0346), `xnu/patches/0304-starlte-m3-cores.patch`, `build/xnu-12377/config/MASTER.arm64.starlte`, `build/dsc-isa-census-macos27.txt`
- `ref/exynos9810-woa/` (PlatformBootManagerLib.c, Madt.aslc, driver INFs)
- `device/dtb/starlte.dts`, `device/pit.txt`, `device/logs/sboot-log.txt`, `build/linux-bundle/sboot.dts`, `build/linux/kernel.config`, `build/em-research-battery.md`, `build/em-research-thermal-clocks.md`, `backup/20261102-185306/`, `backup/s9b/pit.txt`, `build/ARCHIVED-TO-USB.txt`

### 9.2 Apple (macOS, Snapintosh, EL2)

- Snapintosh: https://github.com/Snapintosh/Snapintosh and its docs ARCHITECTURE.md, BOOT-CHAIN.md, RUNNING.md, KERNEL-PATCHES.md, COMPATIBILITY.md, APPLE-ENCODINGS.md, STATUS.md, GUI-BOOT-ROADMAP.md (under `/blob/main/docs/`); https://github.com/Snapintosh/q1n1; https://github.com/Snapintosh
- Coverage: https://videocardz.com/newz/modder-ports-macos-27-to-snapdragon-x2-laptop-now-available-for-download · https://videocardz.com/newz/snapintosh-project-makes-macos-27-run-on-snapdragon-x2-elite-extreme-laptop · https://exceldisc.com/blog/snapintosh-source-code-released-letting-users-run-macos-27-on-snapdragon-x2 · https://cloudnews.tech/snapintosh-brings-macos-27-to-a-laptop-with-a-snapdragon-x2-and-an-adreno-gpu/ · https://www.neoteo.com/en/snapintosh-runs-macos-on-one-snapdragon-laptop-with-a-mac-host · https://www.tweaktown.com/news/114200/developer-runs-macos-27-on-asus-zenbook-a16-with-snapdragon-x2-elite-extreme-and-gpu-acceleration/index.html
- EL2 on Snapdragon: https://github.com/TravMurav/slbounce · https://github.com/TravMurav/Qcom-Secure-Launch/
- Samsung RKP research: https://projectzero.google/2017/02/lifting-hyper-visor-bypassing-samsungs.html · https://census-labs.com/resources/samsung-hypervisor-rkp-arbitrary-zero-write · https://www.exploit-db.com/papers/48073 · https://nvd.nist.gov/vuln/detail/CVE-2019-20556
- MacBook Neo / 26.5: https://ipsw.me/install/Mac17,5/25F71 · https://theapplewiki.com/wiki/Mac17,5 · https://en.wikipedia.org/wiki/Apple_A18 · https://www.costco.com/p/-/macbook-neo-laptop-13-inch-apple-a18-pro-chip-built-for-apple-intelligence-6-core-cpu-5-core-gpu-8gb-memory-256gb-ssd-storage/4000447603
- macOS 26 VM RAM: https://applech2.com/archives/20260117-parallels-install-macos-26-tahoe-vm-on-intel-mac.html
- XNU tags: `git ls-remote --tags https://github.com/apple-oss-distributions/xnu` (newest xnu-12377.121.6)

### 9.3 Google (Android 17, One UI, ChromeOS, Googlebook)

- GSI: https://developer.android.com/about/versions/17/gsi-release-notes · https://developer.android.com/about/versions/17/qpr1/gsi-release-notes · https://developer.android.com/about/versions/17/qpr2/gsi-release-notes · https://developer.android.com/topic/generic-system-image/releases · https://developer.android.com/topic/generic-system-image · https://source.android.com/docs/core/tests/vts/gsi
- AOSP build: https://android.googlesource.com/platform/build/+/refs/heads/main/target/product/aosp_arm64.mk · https://android.googlesource.com/platform/build/+/refs/heads/main/target/board/generic_arm64/BoardConfig.mk · https://android.googlesource.com/platform/build/+/refs/heads/main/target/board/BoardConfigGsiCommon.mk · https://source.android.com/docs/setup/start/requirements · https://source.android.com/docs/core/architecture/vndk
- BPF loader gates: https://android.googlesource.com/platform/packages/modules/Connectivity/+/refs/heads/main/bpf/loader/NetBpfLoad.cpp · https://android.googlesource.com/platform/packages/modules/Connectivity/+/03a10fd14d/bpf/loader/netbpfload.rc
- AOSP source schedule: https://www.androidauthority.com/aosp-source-code-schedule-3630018/ · https://piunikaweb.com/2026/01/07/android-aosp-source-code-q2-and-q4-pixel-monthly-security-patches-unchanged/
- krazey90: https://github.com/ExyHyperBrick/android_kernel_samsung_exynos9810 · https://github.com/ExyHyperBrick/android_device_samsung_exynos9810-common · https://github.com/ExyHyperBrick/local_manifests · https://xdaforums.com/t/rom-s9-s9-note9-unofficial-lineageos-24-0-volte-vowifi-ota-2026-10-09.4763471/
- Old kernels and Android: https://www.androidsage.com/2026/10/12/lineageos-24-released-based-on-android-17-in-browser-webusb-flashing-canvas-photo-editor-and-kernel-overhauls/ · https://dev.to/gde/android-16-on-a-49-kernel-lineageos-232-on-the-pixel-3a-5e79 · https://xdaforums.com/t/rom-s9-s9-n9-unofficial-infinity-x-3-10-android-16-qpr2-volte-vowifi-31-05-2026.4790371/
- Community GSIs: https://github.com/ponces/treble_aosp · https://github.com/TrebleDroid/treble_experimentations · https://github.com/MisterZtr/LineageOS_gsi
- Play Protect: https://support.google.com/android/answer/10248227 · https://www.google.com/android/uncertified/warningauto · Pixel Launcher on AOSP: https://github.com/opengapps/aosp_build/issues/276
- One UI: https://github.com/Eend15/Unofficial-Un1ca-9810 · https://github.com/salvogiangri/UN1CA · https://github.com/rabitxm/star2lte-a376b-port · https://github.com/duhansysl/exynos9810-kernel/releases/tag/V1.9 · https://newreleases.io/project/github/duhansysl/exynos9810-kernel/release/V1.12 · https://www.91mobiles.com/hub/one-ui-9-stable-update-rollout-galaxy-s26-series/ · https://www.androidsage.com/2026/05/19/stable-one-ui-8-5-live-for-galaxy-s25-s24-s23-series-fold-phones/
- ChromiumOS / ChromeOS: https://storage.googleapis.com/chromiumos-image-archive?prefix=arm64-generic-public/ · https://dl.google.com/dl/edgedl/chromeos/recovery/recovery2.json · https://chromium.googlesource.com/chromiumos/overlays/board-overlays/+/HEAD/overlay-arm64-generic/ · https://chromium.googlesource.com/chromiumos/overlays/board-overlays/+/HEAD/ · https://chromium.googlesource.com/chromiumos/overlays/chromiumos-overlay/+/HEAD/media-libs/mesa-panfrost/ · https://chromium.googlesource.com/chromiumos/overlays/chromiumos-overlay/+/HEAD/sys-kernel/ · https://chromium.googlesource.com/chromiumos/overlays/chromiumos-overlay/+/HEAD/chromeos/binhost/target/ · https://chromium.googlesource.com/chromiumos/platform/minigbm/+/HEAD/dumb_driver.c · https://chromium.googlesource.com/chromiumos/platform/minigbm/+/HEAD/drv.c · https://www.chromium.org/chromium-os/developer-library/guides/development/developer-guide/ · https://groups.google.com/a/chromium.org/g/chromium-os-discuss/c/eys9ATHiZKc · https://chromium.googlesource.com/chromiumos/platform/crosutils/+/refs/heads/master/build_library/disk_layout_v2.json
- Brunch and ARM: https://github.com/sebanc/brunch · https://github.com/carsv-inc/chromium_os-rk3588
- Panfrost: https://lkml.iu.edu/hypermail/linux/kernel/2104.2/04707.html · https://docs.mesa3d.org/drivers/panfrost.html · https://cros.tech/device/krane
- Googlebook / Aluminium: https://en.wikipedia.org/wiki/Googlebook · https://www.xda-developers.com/googlebook-is-finally-here-aims-blends-android-chromeos-one-premium-laptop-operating-system/ · https://www.theshortcut.com/p/googlebooks-magic-touch-quick-insert-rambler-gemini-ai-linux-acer-asus-hp-lenovo-dell · https://futurumgroup.com/insights/snapdragon-x-elite-expands-qualcomms-pc-reach-with-googlebook/ · https://petapixel.com/2026/09/21/snapdragon-powered-googlebook-laptops-promise-seamless-connectivity-with-android/ · https://en.wikipedia.org/wiki/Aluminium_OS · https://the-gadgeteer.com/2026/05/16/googlebook-aluminiumos-leak-first-look/ · https://github.com/DangerousAndroid/AluminiumOS · https://aluminium-os.com/aluminium-os-leak-before-io-2026-event/ (returned 403; claims from a search snippet)
- Chrome for ARM64 Linux: https://www.techrepublic.com/article/news-google-chrome-arm64-linux-support-2026/

### 9.4 Microsoft (Windows 11, Windows 10, GPU)

- GPU: https://github.com/gahingwoo/wddm-mali-g52 · https://github.com/gahingwoo/wddm-mali-g52/blob/main/docs/PLAN.md · https://www.phoronix.com/news/Mesa-Drops-Direct3D-10-UMD · https://developer.arm.com/downloads/-/Mali%20GPUs%20User-Space%20Binary%20Drivers · https://blog.getutm.app/2026/introducing-triton-directx-11-driver-for-qemu/ · https://github.com/microsoft/Windows-driver-samples/tree/main/video/KMDOD
- Windows 10 media and support: https://uupdump.net/known.php?q=19045 · https://uupdump.net/selectlang.php?id=9a3b56c6-e8d9-4ec8-b309-e229e47ee772 · https://learn.microsoft.com/en-us/answers/questions/596334/windows-10-arm-iso-download · https://learn.microsoft.com/en-us/windows/iot/iot-enterprise/whats-new/windows-iot-enterprise-ltsc · https://www.helpnetsecurity.com/2026/06/26/microsoft-windows-10-free-security-updates-esu-program/ · https://learn.microsoft.com/en-ca/answers/questions/5805066/windows-esu-does-enrollment-end-on-13-oct-2026-or
- VHDX boot: https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/boot-to-vhd--native-boot--add-a-virtual-hard-disk-to-the-boot-menu · https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/deploy-windows-on-a-vhd--native-boot · https://technet.microsoft.com/library/dd440865
- Arm features: https://learn.microsoft.com/en-us/windows/arm/apps-on-arm-x86-emulation · https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-isprocessorfeaturepresent · https://www.neowin.net/news/first-generation-windows-on-arm-pcs-will-not-be-able-to-run-windows-11-24h2/ · https://lkml.iu.edu/2203.2/02099.html
- USB: https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/usb-dual-role-driver-stack-architecture · https://msdn.microsoft.com/en-us/windows/hardware/drivers/usbcon/oem-tasks-for-usb-type-c-systems-running-windows
- Driver builds: https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-nuget · https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/building-arm64-drivers
- Snapdragon phones: https://www.xda-developers.com/renegade-project-boot-arm64-windows-10-11-on-android-devices/ · https://en.wikipedia.org/wiki/Samsung_Galaxy_S9

### 9.5 Linux (postmarketOS, Ubuntu Touch, modem, Snapdragon S9)

- Ubuntu Touch: https://ubports.com/blog/ubports-news-1 · https://ubports.com/blog/ubports-news-1/ubuntu-touch-24-04-2-0-and-24-04-1-4-release-4007 · https://ubports.com/blog/ubports-news-1/ubuntu-touch-24-04-2-1-and-24-04-1-5-release-4014 · https://ubports.com/blog/ubports-news-1/ubuntu-touch-q-a-195-4009 · https://en.wikipedia.org/wiki/Ubuntu_Touch · https://ubports.com/lomiri-roadmap · https://devices.ubuntu-touch.io/device/pinephone/ · https://forums.ubports.com/topic/11627/upgrading-pinephone-from-20.04-to-24.04 · https://docs.ubports.com/en/latest/_sources/porting/build_and_boot/index.rst.txt · https://ci.ubports.com/job/ubuntu-touch-rootfs/job/main/lastSuccessfulBuild/ · https://repo.ubports.com/dists/ · https://github.com/ubports/ubports-pdk/blob/main/ubuntu-touch-pdk · https://raw.githubusercontent.com/ubports/ubports-pdk/main/scripts/images.sh · https://forums.ubports.com/topic/11975/ubuntu-touch-24.04-amd64-image-availability · https://gitlab.com/ubports/core/rootfs-builder-debos · https://forums.ubports.com/post/85573
- Lomiri on pmOS / Debian: https://nura.eco/blog/2025/01/28/pmOS-update-2025-01/ · https://gitlab.com/postmarketOS/pmaports/-/merge_requests/4496 · https://packages.ubuntu.com/resolute/all/lomiri-greeter · https://packages.debian.org/fr/trixie/mir-platform-graphics-gbm-kms23
- Exynos 9810 and GPU: https://exynos-linux.github.io/ · https://github.com/exynos-linux/samsung-gtactive3wifi · https://lists.freedesktop.org/archives/dri-devel/2021-January/295169.html · https://patchew.org/linux/20250314173858.212264-1-ariel.dalessandro@collabora.com/20250314173858.212264-5-ariel.dalessandro@collabora.com · https://lkml.iu.edu/hypermail/linux/kernel/2410.3/04225.html · https://lkml.iu.edu/hypermail/linux/kernel/2412.1/09806.html · https://android-kvm.googlesource.com/linux/+/297939901f382f16ab78a8073cdfb2a6279bb654 · https://github.com/SheroAbi/galaxy-s9plus-mainline-linux · https://github.com/agcarbajo/galaxy-s9plus-mainline-linux
- Modem: https://eighty-twenty.org/2020/09/10/booting-samsung-galaxy-s7-modem · https://github.com/manoedinata/samsungipcd · https://github.com/manoedinata/libsamsung-ipc · https://gitlab.com/postmarketOS/pmbootstrap/-/issues/598 · https://redmine.replicant.us/projects/libsamsung-ipc/wiki/Wiki/9 · https://lists.osuosl.org/pipermail/replicant/2021-August/003534.html · https://github.com/derekross/linux-omarchy-mobile/pull/144 · https://github.com/h4rkl/omarchy-ferrix/issues/1 · https://github.com/m8l8th814n-eng/tinycbd · https://i.blackhat.com/USA-20/Wednesday/us-20-Hernandez-Emulating-Samsungs-Baseband-For-Security-Testing.pdf · https://postmarketos.org/blog/2021/10/10/v21.06.3-release/ · https://gitlab.com/postmarketOS/pmaports/-/issues/1338
- Snapdragon S9 (MAX77705, panel): https://patchew.org/linux/20250225-starqltechn._5Fintegration._5Fupstream-v9-0-a5d80375cb66@gmail.com/ · https://patchew.org/linux/20250123-starqltechn._5Fintegration._5Fupstream-v17-0-8b06685b6612@gmail.com · https://patches.linaro.org/project/linux-leds/cover/20250123-starqltechn_integration_upstream-v17-0-8b06685b6612@gmail.com/ · https://lkml.iu.edu/hypermail/linux/kernel/2410.1/02849.html · https://lkml.org/lkml/2025/9/25/1571 · https://github.com/me-ramoops/samsung-sdm845-debian · https://www.mail-archive.com/dri-devel@lists.freedesktop.org/msg509483.html · https://gitlab.com/sdm845-mainline/linux/-/merge_requests/72
- The postmarketOS wiki page for samsung-starlte is behind bot protection and was not read.

### 9.6 Starbite OSes and the Installer

- an earlier port (facts only, APSL code not used): https://github.com/example-project/refport-starlte · https://github.com/example-project/refport-starlte/pull/25 (modem plan); SENSORS.md, AUDIO.md, CAMERA.md, BLUETOOTH.md, WIFI.md, `firmware/sensorhub/README.md` in the same repo
- Samsung kernel config: https://github.com/LineageOS/android_kernel_samsung_exynos9810 (lineage-20, `exynos9810-starlte_defconfig`)
- USB: https://lists.denx.de/pipermail/u-boot/2025-October/601405.html · https://docs.u-boot.org/en/v2026.01/board/samsung/exynos-mobile/exynos7870.html · https://github.com/hathach/tinyusb · https://github.com/Benjamin-Dobell/Heimdall · https://www.phoronix.com/news/Generic-USB-Display-GUD-5.13 · https://docs.embassy.dev/embassy-usb/git/default/class/gud/device/index.html · https://git01.mediatek.com/plugins/gitiles/filogic/uboot/+/894cf72c2bff462b29e8e37402ca6f08a77b66fd · https://docs.u-boot-project.org/en/latest/usage/cmd/ums.html
- NFC and GNSS: https://lore.kernel.org/linux-devicetree/CAEx-X7cufBcO38RR2SQzvTOA=JRyG7XSR-KNMcui99WGrsvfZw@mail.gmail.com/T/ · https://www.slideshare.net/slideshow/introducing-samsung-galaxy-s9s9/92050028 · https://github.com/derekross/linux-omarchy-mobile/pull/147
- Location data: https://en.wikipedia.org/wiki/OpenCelliD · https://source.coop/smartmaps/opencellid
- Networks in Finland: https://www.mobileeurope.co.uk/?p=34782 · https://www.telia.fi/artikkelit/artikkeli/3G-shutdown-start-newsroom
- Safety and law (stable references, not re-fetched): https://www.w3.org/WAI/WCAG21/Understanding/three-flashes-or-below-threshold.html · https://www.legislation.gov.uk/ukpga/2002/31 · EU RED 2014/53/EU · 47 U.S.C. §333
- Flashing tools: https://github.com/Samsung-Loki/Thor · https://www.softpedia.com/get/Mobile-Phone-Tools/Others/Odin-3.shtml · https://www.thecustomdroid.com/download-samsung-usb-driver/ · https://community.e.foundation/t/know-your-hardware-samsung/28639/9
- OEM lock, FRP, KnoxGuard: https://www.thecustomdroid.com/prenormal-rmm-state-on-samsung-galaxy-guide/ · https://community.e.foundation/t/s9-bootloader-unlocked-yet-twrp-still-wont-install/27410 · https://community.e.foundation/t/custom-binary-blocked-by-oem-lock-after-parental-control-turn-off/59696 · https://community.e.foundation/t/samsung-galaxy-s9-only-official-released-binaries-are-allowed-to-be-flashed-recovery/27028 · https://www.ifixit.com/Answers/View/375341/Phone+boot+loop,+Custom+binary+blocked+by+FRP+lock
