# Starbite 2: many operating systems, a new boot menu, Starbite Lab

Plan written 2026-11-02 after a full review of the boot chain, storage and every port
(NOTES.md, shim/starlte, tools/*). It says what exists, what blocks growth, the new architecture,
and the order of work. No time estimates: each phase ends when its "done when" holds.

## 1. Where things stand

| OS | How it boots | Root lives on | State |
|---|---|---|---|
| iOS 26.6.2 (iPhone 11 identity, xnu-12377) | XNU payload in BOOT | USERDATA 0-16 GiB (HFSX) | Boots, Metal on the Mali, 8 cores. Black wallpaper, CC tiles faint, some icons plain. Daily image `out/xnu-boot-ios26-daily.img` |
| Windows 11 23H2 (22631) | Our bootshim + SEC/PEI + the woa **binary** DXE, packed in BOOT | WINESP + WINDOWS (UFS) | Desktop + touch. No GPU, USB, Wi-Fi, audio, battery; 4 CPUs |
| Linux 7.1 / postmarketOS edge (Plasma Mobile) | `linux.image`/`linux.dtb` from the UFS OS store | 8 GiB image file on the microSD (exFAT) | Boots: 8 cores, Panfrost GPU at 60 Hz, Wi-Fi, USB networking |
| Android 17 (LineageOS 24) | `android.kernel`/`ramdisk` from the OS store | SYSTEM/VENDOR/ODM + A16DATA (8 GiB) | Installed, **never confirmed booting** |
| Linux kiosk (Linux 4.9 kiosk) | Kernel packed in BOOT (26.4 MB) | USERDATA +17 GiB (ext4) | Boots, but only from Engineer (the menu card is an ad) |
| iPadOS 18.7.11 | - | **gone** (overwritten by iOS 26, no backup) | Needs re-fetch + rebuild |
| iOS 16.7.16 | - | Only as a backup image on the 64 GB card | Needs an Apple root slot |

Building blocks we already own: four boot methods in the loader (XNU, Linux Image+DTB, Android
kernel+ramdisk, UEFI); a generic CRC-checked OS store; UFS reader; FDT editing; ACPM power rails;
PMIC clock; touch; the Mali job path in XNU (StarbiteMali) and Mesa/PanVK/Metal on iOS; mainline
Panfrost on Linux; the arm64e and LSE translators; EngineerMode's hardware pages; pmsg log ring.

## 2. What blocks growth (from the review)

1. **BOOT is full**: 55 MiB, 2.7 MB free. Kiosk's kernel takes 26 MB of it for an entry the menu
   cannot even start. The iOS kernel already ships stripped to fit.
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
- **BOOT keeps only the loader + menu assets + the iOS 26 payload**. Kiosk's kernel and the
  Windows UEFI move to the store. That frees ~34 MB and lets the full (unstripped) kernel back in.

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
- **Storage**: GPT viewer, partition health, store browser, sector viewer (all read-only).
- **USB Lab** (phase 4): show the fastboot status and transfers live.
- **Demos** launch from here too (section 6, Lab category).
- Consistent new UI (the boot menu's design language), touch-first, keys as backup.

## 6. The OS catalog: what goes in each category

Status marks: **[easy]** reuses what works, **[work]** needs real porting, **[stretch]** research.

### Apple
- iOS 26.6.2: daily driver (done; keep improving graphics and speed).
- Darwin console **[easy]**: XNU + a tiny init + a shell from the iOS userland, a pure Darwin
  terminal on the panel (no SpringBoard). Great for development too.
- iOS 16.7.16 **[work]**: the root exists on the 64 GB card. Needs a second Apple root slot (on
  the microSD once XNU can read it, or a UFS slot after repartitioning).
- iPadOS 18.7.11 **[work]**: re-fetch, decrypt, rebuild (the tools exist), then its own slot.
- tvOS 26 **[stretch]**: arm64e like iOS 26 (our translator applies), TV UI controlled by keys
  as a remote. Needs an Apple TV device identity.
- watchOS **[stretch]**: arm64_32 userland, a different ABI; research only.

### Microsoft
- Windows 11 23H2: done (desktop + touch).
- UEFI Shell **[easy]**: already ran on this firmware; as a menu entry.
- Windows PE ARM64 **[easy-work]**: same firmware, boots from FAT (WINESP or a store entry); a
  fast-booting rescue/test Windows.
- Windows 11 22621.2428 **[work]**: the woa release has its firmware; needs its own install.
- Other Windows builds (24H2+, 10) **[stretch]**: each needs a DXE matched to its winload/ntoskrnl.
- Drivers to add on the way: USB (XHCI in the FDF), then GPU is a long shot.

### Android
- LineageOS 24 / Android 17 **[easy-work]**: confirm it actually boots, fix what doesn't.
- **Android versions via GSIs** **[work]**: Generic System Images (Android 12-17, AOSP and
  variants) on top of Lineage's vendor (Treble). System images on the microSD, data in A16DATA or
  on the card. One catalog entry per GSI. This is how "lots of Android versions" becomes real.
- Android Go / microG builds as variants **[work]**.

### Linux
- postmarketOS edge, Plasma Mobile: done.
- **More distros sharing the mainline kernel** **[easy]**: the loader passes
  `starbite.root=<file>`; the init picks that image on the card. Candidates: postmarketOS with
  Phosh / GNOME Mobile / Sxmo, Alpine console, Debian, Arch Linux ARM, Fedora, Ubuntu (desktop
  ARM64 userland), Kali.
- Linux "retro station" **[easy-work]**: QEMU under Linux running Windows 95/XP/98, classic Mac
  OS (via QEMU PPC), DOS games, as their own entries.
- Kiosk/media entries: Linux kiosk (move its kernel to the store), Kodi media center **[work]**.

### Lab and other systems
- **Bare-metal demos inside the loader** **[work, fun]**: Mali GPU spinning 3D/raymarching demo
  using our own GPU job path, Mandelbrot zoom, a particle sandbox with multi-touch, a chiptune
  player on the speaker if audio is reachable, DOOM (bare-metal port, runs off the store's WAD).
- Other OSes **[stretch]**: NetBSD arm64 via our UEFI (it supports ACPI), FreeBSD/OpenBSD,
  Haiku arm64, SerenityOS aarch64, Redox, Plan 9: each a research item; NetBSD is the most
  promising first.
- "Starbite OS": a tiny OS of our own on top of the loader **[stretch]**.

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

**Phase 0: safety and the 128 GB card**
- Back up to the 128 GB card: the iOS 26 root, EFS/CPEFS, the VG6 boot LUNs, the current GPT,
  and copy everything from the 64 GB card (Windows VHDX, iOS 16 root, Linux image).
- Partition the 128 GB card (p1 exFAT, p2 raw store) from TWRP or Linux.
- Done when: every irreplaceable image exists in at least two places.

**Phase 1: loader 2 core**
- Store v2 (UFS + microSD reader), catalog format and tool, payload-independent bundle/store
  lookup, fail back to menu, MMU and caches on, settings record + generic boot-next mailbox.
- Move Kiosk and the Windows UEFI out of BOOT.
- Done when: today's five OSes boot from catalog entries and BOOT has >30 MB free.

**Phase 2: menu v6**
- Categories, run-time labels, scrolling, per-entry options, status chips, boot animations.
- Done when: the menu shows every catalog entry under its category, and adding one needs no
  loader rebuild.

**Phase 3: fill the catalog with the [easy] entries**
- Darwin console, UEFI Shell, WinPE, Linux distros on the card, Kiosk as a normal entry,
  verify Android 17, first bare-metal demo.
- Done when: each category has at least two working entries.

**Phase 4: Starbite Lab + fastboot**
- New Lab UI and pages (dashboard, playground, benchmarks, OS manager); USB device mode and the
  fastboot subset.
- Done when: `fastboot boot` starts an iOS payload sent from the Mac.

**Phase 5: the big ports, one at a time**
- Android GSIs, iPadOS 18 and iOS 16 slots (XNU microSD driver), tvOS, more Windows builds,
  NetBSD, the GPU demo. Meanwhile iOS 26 keeps improving (wallpaper, Control Center, widgets,
  big-core speed).

## 9. Decisions for you
1. Starbite card: keep as a banner on the main menu, or move it into the Lab/Linux category?
2. Samsung's logo: replace with our own full-screen menu art (recommended), or keep drawing around it?
3. Which [stretch] OS excites you most (tvOS, NetBSD, Windows 10, Haiku)? That one goes first in phase 5.
4. The 64 GB card: keep it as the Windows/backup card, or merge everything onto the 128 GB card?
