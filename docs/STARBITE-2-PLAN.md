# Starbite 2: many operating systems, a new boot menu, Starbite Lab

Plan written 2026-11-02 (revised the same evening with your ideas) after a full review of the boot chain, storage and every port
(NOTES.md, shim/starlte, tools/*). It says what exists, what blocks growth, the new architecture,
and the order of work. No time estimates: each phase ends when its "done when" holds.

## 0. The user's priorities (2026-11-03)
- **Apple:** iOS 14 as the stable one, the rest as planned, and later **many iOS versions** plus fun ones (iOS 7, tvOS, iPadOS…).
- **Microsoft:** **both Windows 10 and Windows 11.** Windows 10 ARM64 (ARMv8.0) can use all 8 cores; 11 24H2+ only the 4 A55s.
- **Android:** **lots and lots of Android versions** (GSIs from the card). Android 17 is the main OS on UFS; its data partition size (24 or 32 GB) is decided at the UFS re-layout.
- **Soon:** **Starbite OS** and **ChromeOS** (ChromiumOS/FydeOS on our mainline kernel).

## 1. Where things stand

| OS | How it boots | Root lives on | State |
|---|---|---|---|
| iOS 26.6.2 (iPhone 11 identity, xnu-12377) | XNU payload in BOOT | USERDATA 0-16 GiB (HFSX) | Boots, Metal on the Mali, 8 cores. Black wallpaper, CC tiles faint, some icons plain. Daily image `out/xnu-boot-ios26-daily.img` |
| Windows 11 23H2 (22631) | Our bootshim + SEC/PEI + the woa **binary** DXE, packed in BOOT | WINESP + WINDOWS (UFS) | Desktop + touch. No GPU, USB, Wi-Fi, audio, battery; 4 CPUs |
| Linux 7.1 / postmarketOS edge (Plasma Mobile) | `linux.image`/`linux.dtb` from the UFS OS store | 8 GiB image file on the microSD (exFAT) | Boots: 8 cores, Panfrost GPU at 60 Hz, Wi-Fi, USB networking |
| Android 17 (LineageOS 24) | `android.kernel`/`ramdisk` from the OS store | SYSTEM/VENDOR/ODM + A16DATA (8 GiB) | Boots from the Starbite menu (user, 2026-11-03); a logged first-boot run (docs/ANDROID-PLAN.md) is still useful for bugs |
| iPadOS 18.7.11 | - | **gone** (overwritten by iOS 26, no backup) | Needs re-fetch + rebuild |
| iOS 16.7.16 | - | Only as a backup image on the 64 GB card | Needs an Apple root slot |

Building blocks we already own: four boot methods in the loader (XNU, Linux Image+DTB, Android
kernel+ramdisk, UEFI); a generic CRC-checked OS store; UFS reader; FDT editing; ACPM power rails;
PMIC clock; touch; the Mali job path in XNU (StarbiteMali) and Mesa/PanVK/Metal on iOS; mainline
Panfrost on Linux; the arm64e and LSE translators; EngineerMode's hardware pages; pmsg log ring.

## 2. What blocks growth (from the review)

1. **BOOT was full**: 55 MiB, 2.7 MB free. Kiosk is gone (2026-11-02): 30 MB free now. The iOS kernel
   still ships stripped.
2. **UFS is fully allocated**: no free partition space; the OS store is a fixed 1 GiB (63 entries).
3. **The menu is hard-coded in four places** (menu.mjs labels as pixels, menu.c `v5_os[5]`, em.c,
   xnu_boot.c dispatch), only 4 rows fit above Samsung's logo, labels are baked into sprites.
4. **Every OS depends on a valid iOS payload** (bundles are found from the payload's end) and every
   failure ends in Download mode instead of back at the menu.
5. **No USB in the loader**: the dev loop is TWRP + adb dd + a reboot per test (8 min per iOS test).
6. **The loader runs with the MMU and caches off**: every pixel is an uncached DRAM write, so
   Engineer redraws are slow.
7. **No microSD reads in the loader, XNU or Windows**: the 128 GB card can't hold boot files yet.
8. **Backup gaps**: no copy of the iOS 26 root on any card, the Windows VHDX exists only on the
   64 GB card, the VG6 boot LUNs and the post-split GPT have no off-phone backup beyond the Mac.

## 3. The new architecture

```
S-Boot ──> BOOT partition (stays small and stable)
             stage 1: Starbite loader 2  (+ fonts, icons, Lab)
                │  reads the catalog + payloads from:
                ├── UFS OS store (1 GiB, as today)      - what must work without a card
                └── microSD "OS library" (128 GB card)  - everything else
                │
                └── boots via one of the generic methods:
                     xnu | linux (Image+DTB+initrd+cmdline) | android (boot.img) | uefi (FD) | bare-metal demo
```

### 3.1 Catalog instead of hard-coded rows
- One **catalog** (a small binary manifest, `catalog.sbc`, built by a Mac tool from a readable
  `catalog.toml`) lists every entry: category, name, version line, icon id, boot method, the store
  names of its files, its root location, its command line, and flags (hidden, experimental).
- The loader draws the menu from the catalog. Labels are rendered at run time with the SBFONT
  atlas we already have; only icons stay as sprites. Adding an OS = adding a catalog entry.
- Because of the no-absolute-relocations rule, the catalog is data parsed at run time, never a
  static pointer table.

### 3.2 Storage
- **OS store v2** (`SBSTORE2`): same idea as today, but 64-bit offsets, more entries and a type field.
  It exists twice: the current 1 GiB UFS region (kept for iOS 26, Windows, recovery-critical
  entries) and a raw partition on the 128 GB card (`p2`, e.g. 32 GiB) for the OS library.
- **microSD layout (128 GB SanDisk)**:
  - `p1` exFAT "STARBITE" (~90 GB): Linux root images (one file per distro), Android GSI
    images, Windows install media and VHDXs, backups.
  - `p2` raw `SBSTORE2` (~32 GB): kernels, DTBs, ramdisks, XNU payloads, UEFI FDs, demo
    binaries, the catalog, menu settings.
- The loader gets a **polling DW-MMC read driver** (the rails are already powered by us; XNU's
  `a5xelte_emmc.c` and Linux's dw_mmc patches are the references). Read-only, like the UFS one.
- **BOOT keeps only the loader + menu assets + the iOS 26 payload**. Kiosk is removed; the
  Windows UEFI moves to the store too. That leaves ~37 MB free and lets the full kernel back in.

### 3.2b UFS re-layout: Android as the main OS
- Android 17 gets **A16DATA = 32 GB** (from 8 GiB). UFS is full, so something moves to the card:
  the **Apple roots move to the microSD library**, which needs an **XNU microSD driver** (the
  key enabler: it also gives every Apple OS its own root on the card). Windows stays on UFS
  (it has no SD driver), shrunk to ~20 GB; the 1 GiB OS store stays.
- Card speed: today high-speed mode (22.6 MB/s); UHS (1.8 V switch on LDO2) brings ~80 MB/s.
- Order: back up everything, add the XNU SD driver, move iOS 26's root to the card and prove it,
  then repartition (our own GPT tool, keeping entries 1-24 and the Windows GUIDs).

### 3.3 Robustness
- Bundles and the store are found independently of the iOS payload: a broken iOS no longer
  takes the menu down.
- Any load error returns to the menu with the reason on screen (Download mode only on request).
- A general **"boot entry X next"** mailbox (today's `SBNEXTLX` only knows Linux), so any OS or
  the Mac can choose the next boot. Last choice, default entry and timeout persist in a settings
  record (no `starauto` in command lines any more).
- Never-touch list enforced in every writer we own: EFS, CPEFS, boot LUNs, PARAM/UP_PARAM,
  KEYSTORAGE, PERSISTENT, STEADY, BOTA, DTB/DTBO, the GPT/PIT region. No Odin/Heimdall by name for
  anything at SYSTEM or beyond (the PIT no longer matches the GPT).

### 3.4 Speed
- Turn on the MMU and caches in the loader (identity map, framebuffer write-combined). Every
  menu and Lab redraw becomes instant, and it's a prerequisite for animation in the menu.

## 4. The boot menu (v6)

- Full-screen, our own artwork (no drawing around Samsung's logo), Starred Wordmark style.
- **Categories as tabs**: Apple · Microsoft · Android · Linux · Lab. Volume keys or swipe
  between categories, scrolling list of entries inside each, Power or tap to boot.
- Each entry: icon, name, version line, a status chip (Daily / Beta / Experimental / Not
  installed), and its last boot result ("booted 12:03", "failed: kernel CRC").
- Long-press an entry: per-entry options (boot args, CPU count, verbose, safe mode), saved.
- Bottom: Recovery, Download, Lab, Power off, plus the Starbite card kept as a compact banner.
- Boot animation per OS while its files load (progress from the store reads).
- **Full touch** everywhere (taps, swipes between categories, long-press, scrolling) and
  **GPU-drawn**: first MMU+caches (smooth software drawing), then the Mali through our own job
  path (the code that drew XNU's first triangle) for animation, blur and transitions.

## 5. Starbite Lab (EngineerMode reimagined)

Today's Engineer is a 2012-style Holo clone of read-only hex pages. Lab becomes a toolbox you
want to open:

- **Dashboard**: live CPU clusters and clocks, temperatures (TMU), battery and charger graph,
  RAM, uptime; animated, smooth (needs the MMU/caches work).
- **Hardware playground**: multi-touch paint, display colour/burn-in/uniformity tests,
  brightness ramp, keys, vibration motor, flashlight LED, sensors if reachable, speaker beep.
- **Benchmarks with history**: CPU per core (A55 vs M3), RAM bandwidth, UFS and microSD speed,
  **Mali GPU benchmark** (our own job submission: the same code that drew the first triangle in
  XNU, ported to the loader).
- **OS manager**: browse the catalog, verify every file's CRC, set default/timeout, edit
  boot args, see last boot logs (pmsg ring, XNU log ring), boot any entry.
- **Every OS**: per-OS pages (iOS root health, boot args, logs, overlay files; Windows BCD/latch
  records; Android cmdline/props; Linux store entries), install/update/remove an OS.
- **Storage**: GPT viewer and **editor**, partition health, store browser, sector viewer and
  **editor**, image backup/restore to the card or USB stick.
- **Armed mode** (it's Engineer mode: it may do anything): writes to any partition, register
  pokes, PMIC rails, clocks, fuses-read, flashing. Every dangerous action shows exactly what it
  will change, needs a deliberate confirmation (hold + slide), and takes an automatic backup
  first. EFS/CPEFS/boot LUNs stay behind a second, explicit unlock.
- **USB Lab** (phase 4): show the fastboot status and transfers live.
- **Demos** launch from here too (section 6, Lab category).
- Consistent new UI (the boot menu's design language), touch-first, keys as backup.

## 6. The OS catalog: what goes in each category

Every entry carries a **tag** in the menu: **Daily** (use it every day), **Beta** (works, rough
edges), **Experimental** (boots, many gaps), **Lab** (demo/novelty), **Not installed**.
Feasibility marks below: **[easy]** reuses what works, **[work]** real porting, **[stretch]** research.

### Apple
| Entry | Tag goal | Notes |
|---|---|---|
| iOS 26.6.2 | Beta | Today's port (Metal on the Mali, 8 cores). Keep improving: wallpaper, Control Center, widgets, speed |
| **iOS 14** | **Daily (the stable Apple OS)** | **[work]** iPhone 6s/7 identity: their userland is plain arm64 (no PAC), so **no translator and no instruction emulation**: it runs natively on all 8 cores. Pre-sealed-system-volume, so the root is ordinary. Kernel: xnu-7195 (public source), ported back from our 8796 series. Graphics: iOS 14 still has the OpenGL ES CoreAnimation path we already drove with Mesa on iOS 16/iPadOS 18, which is far simpler than Metal |
| iPadOS (big screen) | Beta | **[work]** iPadOS 18.7.11 ran before (tools exist; root needs rebuilding), or iPadOS 26 on our iOS 26 stack with an iPad identity and the full 1440x2960 canvas |
| tvOS | Lab | **[stretch]** tvOS 26 is arm64e like iOS 26 (our translator applies); TV UI driven by keys/touch as a remote |
| Darwin console | Lab / dev | **[easy]** XNU + a tiny init + a shell, a pure Darwin terminal; also the base for "PureDarwin"-style experiments |
| iOS 7 | Lab (history) | **[stretch]** Replaces iOS 6 (2026-11-02): iOS 7 was the first 64-bit iOS (iPhone 5s, A7), so its arm64 userland needs no 32-bit support. The hard part is the kernel: no open-source arm64 XNU is that old (public arm64 sources start around xnu-4570/iOS 11), so either a newer XNU taught iOS 7's syscall/Mach ABI, or Apple's iOS 7 kernelcache patched for the Exynos. Research; after iOS 14 |
| iOS 16 | dropped | Its root stays as a backup only |

### Microsoft
- Windows 11 23H2 (Beta): done (desktop + touch). Next drivers: USB (XHCI), then battery/keys.
- UEFI Shell (Lab) **[easy]**, Windows PE ARM64 (Beta, rescue) **[easy-work]**.
- Windows 11 22621.2428 **[work]**; other builds **[stretch]**, one DXE per build:
  - **Windows 11 24H2/25H2**: they require ARMv8.1 atomics (LSE). The A55 cores have them, the
    M3 cores don't, so they run on the 4 A55 cores only (Windows already runs on 4).
  - **Windows 10 ARM64 (19041/19045)**: ARMv8.0 is enough, so all 8 cores are possible.
  - Where they live: Windows has no microSD driver, so every Windows lives on the UFS WINDOWS
    partition (NTFS), each version as a VHDX with native VHD boot and its own BCD entry; the
    catalog entry picks it (one shared UEFI image in the store).
- **GPU on Windows**: there is no Mali driver for Windows on ARM (Arm ships none; Windows on ARM
  GPUs are Adreno only), so Windows draws in software (WARP) today. Steps: all 8 cores (needs
  the M3 LSE story or Windows 10), a display-only WDDM driver for DECON (vsync, proper modes,
  faster scanout), then a real Mali WDDM driver (kernel driver + D3D user-mode driver, e.g. on
  Mesa's Panfrost/PanVK through a Dozen-like layer) as a long research project.

### Android (the main OS on UFS)
- **LineageOS 24 / Android 17 = the Daily main OS**, on UFS with a **32 GB** data partition
  (section 3.2b). First: confirm it boots and fix what doesn't.
- **More Android versions via GSIs** **[work]**: Generic System Images (Android 12-17, AOSP,
  microG, Go) on top of Lineage's vendor, system images in the microSD library, one entry each.

### Linux
- postmarketOS edge, Plasma Mobile (Beta). **First fix: the GPU.** Panfrost's devfreq sat at
  260 MHz 99.8 % of the time (NOTES.md:1178); pin/tune it to 572 MHz (as iOS runs it), check
  KWin's compositing path, then UHS on the microSD for app start. Goal: 60 fps UI.
- **More distros on the same mainline kernel** **[easy]**: `starbite.root=<file>` picks the
  image on the card: postmarketOS with Phosh/GNOME Mobile/Sxmo, Debian, Arch Linux ARM, Fedora,
  Ubuntu, Kali.
- **ChromeOS** **[stretch]**: ChromeOS Flex is x86-only; the ARM route is ChromiumOS / FydeOS
  built for a generic arm64 board on our mainline kernel, Panfrost for graphics.
- Retro station **[easy-work]**: QEMU under Linux running Windows 95/98/XP, classic Mac OS, DOS.
- Media: Kodi **[work]**. (Linux kiosk is retired.)

### Starbite's own systems
- **Starbite OS** **[work → stretch]**: our own ultra-light OS, not Linux, grown from the loader
  (which already drives UFS, touch, display, keys, PMIC, power rails, clocks and has the Mali job
  path from XNU to port). Adds a scheduler, apps, the GPU UI of the boot menu, USB, and later the
  **modem**: the Shannon modem needs Samsung's CP boot (firmware load from RADIO, EFS-backed
  NV data) and its IPC protocol over shared memory. Calls/SMS on our own OS is a long research
  project; open references exist for the IPC side (libsamsung-ipc, ShannonEE research).
- **USB Toolkit** **[easy-work]**: the phone becomes USB devices on the cable: keyboard/mouse,
  mass storage (expose the microSD), network adapter, serial console, MIDI, game controller,
  display-as-webcam. First as a Linux entry (configfs gadgets: USB networking already works
  on our Linux), later native in Starbite OS (the same DWC3 work as fastboot).
- **Demos (Lab)**: Mali GPU 3D/raymarching with our own job path, Mandelbrot, multi-touch
  particle sandbox, DOOM (bare-metal), chiptunes if audio is reachable.

### Other systems [stretch]
NetBSD arm64 via our UEFI (it supports ACPI: the most promising), FreeBSD/OpenBSD, Haiku arm64,
SerenityOS aarch64, Redox, Plan 9.

## 6b. Starbite Installer and the second S9

- **The second, identical S9** is the test device: everything we build gets proven on a clean
  phone, and the daily phone stops being the guinea pig.
- **Starbite Installer** (like a Hackintosh install USB): a USB stick (FAT/exFAT) with an
  installer manifest, the boot image, our recovery and every OS image.
  1. On a stock S9, Download mode accepts a flash of BOOT and RECOVERY. From a computer
     (Heimdall/Odin) **or from the first S9**: Starbite Linux with USB host mode runs Heimdall
     and flashes the second phone, phone to phone.
  2. Our **Starbite Recovery** (replacing TWRP: our kernel + a small installer UI) boots, reads
     the USB stick through the USB-C adapter (USB host/OTG: Type-C role switch + VBUS boost from
     the MAX77705), checks the phone (model, bootloader version, EFS backed up), and lays out
     the partitions and the microSD library.
  3. It installs every OS from the stick and verifies each one.
- Limits: Download mode itself is Samsung's signed bootloader; it can't be modified or run
  our code, so step 1 always needs a flasher (a computer or the other S9). OEM unlock and
  KnoxGuard have to allow it on the new phone (check first).
- Prerequisites: USB host mode on our Linux, a full backup/restore of every partition we touch,
  and the PIT/GPT issue (never let Odin/Heimdall write by name past SYSTEM; the installer
  repartitions itself).

## 6c. What S-Boot shows
- **"Powered by android" and Samsung's splash** come from `logo.jpg` inside the UP_PARAM
  partition (a tar, drawn 1:1 by S-Boot). Replacing that image with Starbite's own splash is a
  known mod. Safe when done with a verified backup of UP_PARAM and our own tar writer.
  UP_PARAM moves from the never-touch list to "touch only with backup + verify".
- The menu then owns the screen from the first frame (and no longer draws around a logo).
- **Download mode, the S-Boot warning screens and S-Boot's own settings can't be changed**:
  they live in the signed bootloader.

## 7. Developer features

- **Fastboot in the menu** (phase 4): bring up the Exynos USB DWC3 in device mode by polling and
  speak the fastboot protocol, so the Mac's `fastboot` works directly with the phone:
  - `fastboot boot <image>`: load an iOS payload / Linux kernel into RAM and boot it **without
    flashing**: the 8-minute TWRP test loop becomes seconds.
  - `fastboot flash <store-entry>` and `flash` to an allowlist of partitions (never the
    never-touch list).
  - `fastboot getvar` (device state, catalog, battery), `oem log` (pmsg ring and XNU log),
    `oem screenshot`, `oem reboot-to <entry>`.
- Mac-side tools: `starbite catalog build`, `starbite store push`, `starbite boot <entry>`,
  one command to rebuild + push + boot any OS.

## 8. Order of work

**Phase 0: safety, the 128 GB card, the second S9**
- Back up to the 128 GB card: the iOS 26 root, EFS/CPEFS, the VG6 boot LUNs, UP_PARAM, the
  current GPT, and everything on the 64 GB card (Windows VHDX, iOS 16 root, Linux image).
- Partition the 128 GB card (p1 exFAT, p2 raw store) from TWRP or Linux.
- Second S9: read its model, bootloader version, OEM-unlock/KnoxGuard state; back up its EFS.
- Done when: every irreplaceable image exists in at least two places, and the second S9's
  state is known.

**Phase 1: loader 2 core**
- Store v2 (UFS + microSD reader), catalog format and tool, payload-independent bundle/store
  lookup, fail back to menu, MMU and caches on, settings record + generic boot-next mailbox.
- Move the Windows UEFI out of BOOT (Kiosk already removed); free Kiosk's old root (USERDATA +17 GiB).
- Done when: today's OSes boot from catalog entries and BOOT has >30 MB free.

**Phase 2: menu v6**
- Full-screen, categories, run-time labels, tags, full touch, per-entry options, boot animations.
- Done when: adding an OS needs only a catalog entry, and every OS is reachable by touch.

**Phase 3: quick wins in every category**
- Linux GPU at full speed, Linux distros on the card, Darwin console, UEFI Shell, WinPE,
  Android 17 confirmed, USB Toolkit (Linux), first demo.
- Done when: each category has at least two working entries.

**Phase 4: Starbite Lab, fastboot, Starbite Recovery + Installer**
- New Lab (all pages, armed mode); USB device mode and the fastboot subset; USB host mode;
  our recovery replacing TWRP; the installer proven on the second S9 from a USB stick.
- Done when: the second S9 goes from stock to full Starbite from a USB stick, and
  `fastboot boot` starts an iOS payload sent from the Mac.

**Phase 5: storage re-layout and the Apple line**
- XNU microSD driver, Apple roots on the card, UFS re-layout (Android 32 GB data), iOS 14
  (xnu-7195, native on 8 cores), iPadOS, tvOS. iOS 26 keeps improving in parallel.

**Phase 6: the long ones, one at a time**
- Starbite OS (own UI on the GPU, USB, then the modem), Android GSIs, ChromiumOS, more Windows
  builds, NetBSD, iOS 6, GPU-drawn boot menu.

## 9. Decisions for you
1. The 64 GB card: keep it as the Windows/backup card, or merge everything onto the 128 GB card?
2. Second S9: which firmware/bootloader is on it, and may it be wiped for the installer tests?
