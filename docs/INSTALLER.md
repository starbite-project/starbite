# Starbite Installer (v1)

Written 2026-11-05. Item 7 of `docs/STARBITE-2-PLAN.md` "Current order": from a Mac or a Linux PC you run
one script; it asks you to confirm an unlocked bootloader and a microSD of at least 64 GB, then makes the
phone match the main Starbite S9, OS by OS, every write verified by reading it back. No user password or
passcode on any OS (section 6). Rescue USB disks for the Bixby > Starbite Recovery > Rescue mode path are
made by the same script (section 7).

**Status: host side done and rehearsed against fake phones and fake disks. Nothing has run against the
real phone.** Section 10 lists exactly what needs the phone.

## 1. Quick start

```sh
tools/installer/starbite-install --bundle BUNDLE                 # = --dry-run: reads the phone, writes nothing
tools/installer/starbite-install --bundle BUNDLE --real          # after a finished dry run; typed confirmation
tools/installer/starbite-install --verify-bundle BUNDLE          # files vs manifest (size + SHA-256)
tools/installer/starbite-install --update-hashes BUNDLE          # whoever builds a bundle: fill size + sha256
tools/installer/starbite-install --describe-phone                # the phone's layout as manifest TOML (read only)
tools/installer/starbite-install --make-rescue-image FILE        # the rescue USB disk as an image file
tools/installer/starbite-install --make-rescue-usb /dev/diskN [--image FILE] [--real]
tools/installer/rehearse.sh                                      # everything below against fakes (no phone)
python3 -m unittest discover -s tools/installer/tests            # unit tests
```

Python 3 standard library only (3.11+ for `tomllib`; older Pythons use the built-in fallback parser).
macOS and Linux. Host tools it calls: `adb` (Android platform-tools), `fastboot` for a phone in the
loader's fastboot, `heimdall` only for a phone in Download mode, `bash`, `gzip`, `shasum`/`sha256sum`.

Options: `--replace KEY` (repeatable) / `--replace-all` overwrite targets that hold data; `--erase-card`
erases a microSD with another layout; `--fresh-backup` takes a new backup even if a complete one exists;
`--rescue-usb DEV` writes the rescue stick at the end; `--state-dir DIR` (default `build/installer/`);
`--no-reboot`; `--yes` (dry run and rehearsal only; refused with `--real`).

## 2. Design

```
Mac / Linux PC                                       Galaxy S9
starbite-install ──adb (TWRP)──────────────────────► reads: getprop, /sys, by-name, MBR, hashes
   │   (fastboot reboot recovery / adb reboot recovery / heimdall --RECOVERY to get there)
   ├─ welcome + 2 confirmations
   ├─ phone state: TWRP | Android | Starbite fastboot | Download mode
   ├─ preflight (read only): model, bootloader, battery, UFS size, protected areas, UFS layout, card size
   ├─ plan screen: every step, what it writes where, by which tool, and why (write/skip/keep/replace)
   ├─ dry run (default)  ── prints every command, writes nothing, records "dry run done" in the journal
   └─ real (--real) / rehearsal (--rehearse, fakes only): typed "install phone-XXXXXXXXXX", then the steps
```

- **Transport: TWRP's adb**, the path every UFS and card write of the project already uses. The loader's
  fastboot (docs/USB-FASTBOOT.md) has no write path by design, so a phone found there is rebooted into
  TWRP (`fastboot reboot recovery`). Android: `adb reboot recovery`. Download mode: TWRP goes into
  RECOVERY with `heimdall flash --RECOVERY twrp.img --no-reboot` (typed "flash twrp"; RECOVERY's stock
  image is in Samsung's firmware), then the user boots TWRP with the keys. A dry run never reboots or
  flashes anything; it then shows the plan without phone checks.
- **Existing tools do the writing** wherever one exists (section 4); the installer adds the checks before,
  the journal, and an independent read-back after each one.
- **Modes.** `--dry-run` is the default. `--real` and `--rehearse` both need a finished dry run of the same
  manifest (by SHA-256) on the same phone (journal), and a typed confirmation. `--rehearse` refuses unless
  the `adb` on PATH is the fake one (`--fake-root DIR` puts `tools/installer/fakes/` first on PATH).
- **Safety rules, enforced in code:**
  - the only way anything is written is an item destination (`sbinst/manifest.py parse_dest`), and the
    grammar cannot name EFS, CPEFS, PARAM, UP_PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA*, DTB, DTBO, the
    raw disks (GPT) or the boot LUNs; ODM only as a fresh `mkfs`; card sector 0 only through the card
    layout step;
  - backup first: the protected areas, BOOT, RECOVERY, MISC, RADIO, OMR, ODM, the first 4 MiB of sda
    (protective MBR, GPT, PIT area), the backup GPT and the boot LUNs sdb-sde go to
    `build/installer/<phone-id>/backups/<time>/`, each hashed on the phone and on the computer;
  - the GPT is checked against the manifest's `[[layout]]` and never written (section 9);
  - data is never replaced silently: an item marked `data = true` (OS roots, Android /data, Windows, card
    images) is written only if its target is blank, or was written by this installer and interrupted, or
    with `--replace KEY` / `--replace-all`. Android /data and /metadata are renewed together or not at all;
  - every write is read back (SHA-256 computed on the phone); a mismatch stops the run (`STOPPED`, exit 3);
  - BOOT is written last, so until the very end the phone still boots TWRP the old way;
  - a microSD with a foreign layout is erased only with `--erase-card` (the plan says BLOCKED otherwise);
  - one phone at a time (`more than one phone` refuses).
- **Idempotent and resumable.** The journal (`build/installer/<phone-id>/journal.json`) records each item as
  (key, destination, SHA-256). A re-run: items done and holding no user data are verified again by hash
  (`skip`), done data items are kept (`keep`, not re-read: their OS has been writing to them), interrupted
  items resume from their chunk state (`chunks/<key>-<sha12>.state`, tied to the content), new items are
  written. Without a journal (a phone installed some other way), targets that already hold the same bytes
  are skipped and everything with data is kept.
- **Phone id** = `phone-` + the first 10 hex digits of SHA-256(serial number): the serial itself is not
  printed or stored.
- Logs: `build/installer/logs/<time>-<mode>.log` (every command and its output).

Files: `tools/installer/starbite-install` (entry), `sbinst/` (`cli.py` options, `steps.py` the flow,
`manifest.py`, `journal.py`, `device.py` adb/fastboot/state/facts/chunk writer, `disks.py` rescue disk
safety, `rescue.py`, `nopassword.py`, `ext4.py` read-only ext4 reader, `tomlmini.py`, `ui.py`),
`installer-manifest.example.toml`, `fakes/` (fake phone, adb, fastboot, heimdall, rehearsal bundle),
`tests/`, `rehearse.sh`.

## 3. The manifest (`installer-manifest.toml`, format 1)

A bundle is a directory: `installer-manifest.toml` plus the files it names. Full example (the main phone's
set): `tools/installer/installer-manifest.example.toml`.

| Key | Meaning |
|---|---|
| `format = 1` | required |
| `name` | shown on the screens |
| `catalog` | optional: a copy of `tools/store/catalog.toml` in the bundle. Then the plan lists every visible menu entry as `covered` or `MISSING` (its store files must be in the bundle's stores, read with `sbstore2.py`; its root must be an item) and warns when the UFS store's catalog differs from it |
| `[phone]` | `models` (SM-G960F), `devices` (starlte), `ufs_blocks` (15616000), `min_battery` (50) |
| `[card]` | `min_bytes` (at least 60,000,000,000: a card sold as 64 GB holds 61.9-64.1 GB), `sectors` (the exact card this layout was made for; card roots sit at fixed sectors in payload command lines), `p1`/`p2` `{type, start, sectors[, label]}`: the MBR the card layout step writes |
| `[[layout]]` | `name`, `node`, `start`, `blocks` (4 KiB blocks): the UFS GPT the set was made for. Checked in preflight, never written |
| `[[item]]` | one thing to install, below |

| Item key | Meaning |
|---|---|
| `key` | unique, `[a-z0-9-]`; `--replace KEY` names it |
| `title`, `os`, `note` | shown; `os` (starbite, android, linux, chromeos, windows, macos, ios) drives the no-password list |
| `file`, `size`, `sha256` | path inside the bundle (no `..`, no symlink out), byte size, SHA-256. `--update-hashes` fills the last two |
| `dest` | `part:NAME` (BOOT RECOVERY SYSTEM VENDOR A16DATA WINESP WINDOWS; ODM only with `action = "mkfs-metadata"`), `slot:USERDATA@OFFSET` (the 16 GiB Apple slot), `store:ufs`, `store:sd`, `card:lba:SECTOR` (inside `[card] p2`, 1 MiB aligned, no overlaps), `card:file:/PATH` (a file on card p1's exFAT) |
| `action` | `write` (default) or `mkfs-metadata` (ODM, no file) |
| `data` | `true`: the target holds user data once its OS runs (never replaced without `--replace`) |
| `optional` | `true`: a missing file skips the item instead of refusing the bundle |

Checks at load: unknown destination or a protected name refuses; sizes must be 4096-aligned for UFS and
MiB-aligned for card ranges; slot items stay below 16 GiB; card ranges inside p2 and not overlapping;
duplicate keys or destinations refuse. `--verify-bundle` then checks presence, size and SHA-256 of every
file (hashes cached in `build/installer/hash-cache.json` by path, size and mtime).

## 4. The steps (fixed order)

| # | Step | Writes | Tool | Verified by |
|---|---|---|---|---|
| 1 | Back up | the computer only | `tools/android/pixel/part-backup.sh mac` per partition; raw head/tail/LUNs by the installer | phone SHA-256 == local SHA-256 for each |
| 2 | UFS layout | nothing | preflight | by-name, node, start, size vs `[[layout]]` |
| 3 | microSD MBR | card sector 0, p2's first 3 blocks (old store header) | installer (`dd` of a 512-byte MBR built on the host), `blockdev --rereadpt` | MBR read back byte for byte; `/sys/class/block/mmcblk0p1/start` |
| 4 | microSD p1 exFAT | p1 | TWRP's `mkfs.exfat` / `mkexfatfs -n STARBITE` | "EXFAT   " read back from p1's boot sector |
| 5 | Apple slot (iOS 16.5) | USERDATA 0..16 GiB | `tools/refport/slot-write.py` | range SHA-256 on the phone |
| 6 | UFS OS store | USERDATA +16 GiB | `tools/store/store-flash.sh` (keeps the phone's settings block) | store area == image except block 2 |
| 7-8 | SYSTEM, VENDOR | partitions | `tools/android/pixel/part-write.py --state` | the tool, then range SHA-256 again |
| 9-10 | A16DATA, ODM | partitions | `part-write.py`; `metadata-reset.sh` (TWRP's mke2fs) | range SHA-256; `blkid` TYPE ext4 LABEL metadata |
| 11-12 | WINESP, WINDOWS | partitions | installer chunk writer | range SHA-256 |
| 13 | card OS store | card p2 | `tools/store/store-flash-sd.sh` | store area == image except block 2 |
| 14-15 | macOS Ventura, iOS 26 roots | card sector ranges | `tools/macos/card-range-flash.py --state` | range SHA-256 |
| 16-18 | pmOS, Ubuntu, ChromiumOS images | files on p1 (`/external_sd/distros/`) | installer chunk writer into `NAME.part`, then `mv` | `sha256sum` of the file |
| 19 | RECOVERY (TWRP) | partition | installer chunk writer | range SHA-256 |
| 20 | BOOT (loader, last) | partition | installer chunk writer | range SHA-256 |
| 21 | No password | nothing | section 6 | |
| 22 | Finish | the rescue stick if `--rescue-usb DEV` | section 7 | read back |

The installer chunk writer (`device.py chunk_write`) is part-write.py's algorithm for the targets the
existing tools do not take: 64 MiB chunks, zero chunks from `/dev/zero` on the phone, others gzip'd,
pushed to `/tmp` and `dd`'d at their offset, each chunk read back and compared, finished chunks recorded.
Targets are unmounted first (`umount`, checked again). The ODM backup in step 1 is what
`metadata-reset.sh` requires (`BACKUP_LOG` points at the backup's `backups.txt`).

Small change to existing tools: `store-flash.sh` and `store-flash-sd.sh` used BSD-only `stat -f %z`;
they now use `wc -c <`, so they also run on a Linux host.

## 5. What a bundle must hold for "every OS working"

The example manifest lists the main phone's set: BOOT, TWRP, both stores (the UFS store with the catalog,
every OS's kernel/payload, Engineer OS and `rescue.image`/`rescue.dtb`/`rescue.initrd`), the iOS 16.5
slot, Android SYSTEM/VENDOR/A16DATA + fresh ODM, Windows (optional), macOS Ventura and iOS 26 (optional)
card roots, and the pmOS, Ubuntu and ChromiumOS images. **No such bundle exists yet**: assembling it from
clean sources (docs/STARBITE-MASTER-PLAN.md 5.5) is its own job, and some parts are personal by licence
(Google's GSI, Apple's roots, Windows). The plan screen's "Menu entries this bundle covers" table says,
per menu entry, whether a given bundle can boot it.

## 6. No user password on any OS

The user's rule: no password or passcode anywhere ("that'll cause confusion"); where an OS insists on an
account, log in automatically. The installer prints this list on the plan screen and checks it again
after the install (`sbinst/nopassword.py`).

| OS | How | Implemented | Checked by the installer |
|---|---|---|---|
| Android 17 | A16DATA is an empty f2fs (`make-a16data.sh`) and ODM (/metadata) is made fresh, together: no lock-screen credential can exist. The setup wizard offers "Screen lock": Skip | yes (fresh-data pairing in the plan) | journal: A16DATA written fresh in this install → `ok`; existing data kept → `OPEN` (a lock set earlier stays) |
| postmarketOS, Ubuntu (Phosh) | user "user" with an **empty** password, and the GNOME/Phosh lock screen off (`org.gnome.desktop.screensaver lock-enabled=false`, `org.gnome.desktop.lockdown disable-lock-screen=true` as a gschema override); greetd/tinydm autologin as before | **new**: `SB_NO_PIN=1` for `mk-payload.sh` writes `pin none`; `sb-integrate.sh` then runs `passwd -d user`, writes `/etc/starbite/login` = `none` and the override (removed again if `glib-compile-schemas` rejects it); `mkroot-pmos.sh` and `check-root.sh` accept and check it. Without `SB_NO_PIN=1` nothing changes (a 6-digit PIN as before) | yes: reads `/etc/shadow` and `/etc/starbite/login` straight out of the ext4 image in the bundle (`sbinst/ext4.py`); a PIN or a locked account → `OPEN` |
| ChromiumOS | test image (cros_debug); Guest mode, no account | by design | listed (`by-design`) |
| Windows 11 | unattend: local account `starbite`, empty password, AutoLogon (`tools/woa/vm/build-image.ps1`) | in the image build | the unattend in the repo has an empty `<Password>` and AutoLogon → `ok`. No clean Windows image exists yet |
| macOS Ventura | auto-login as `starbite` (autoLoginUser + kcpassword) | auto-login yes; **the account has a password (`starbite`)** | `OPEN`, shown on the final screen. Removing it (empty password + kcpassword) touches the working Ventura desktop root and is untested on the phone, so v1 leaves it |
| iOS 16.5 (an earlier port) | no passcode until one is set in Settings | by design | listed (`by-design`) |
| Engineer OS / Starbite | no accounts; armed mode is hold-and-slide | by design | listed |

What cannot be guaranteed from the computer: a setup wizard that offers a PIN (Android) is skipped by
the person at the phone; a lock set later by the user; macOS as above.

## 7. Rescue USB disk

- `--make-rescue-image FILE`: `tools/rescue/mk-rescue-usb.sh` (MBR + FAT32 `SB-RESCUE`,
  `starbite-rescue/rescue.sh` + `rescue.sha256`). On Linux it runs directly (sfdisk, dosfstools, sudo for
  the loop mount); on macOS inside the Lima VM `starbite`, streamed back and its SHA-256 compared with the
  one the VM computed. Refuses `/dev/...` and non-regular files as output.
- `--make-rescue-usb /dev/diskN` (macOS) or `/dev/sdX` (Linux): shows device, model, size, bus, internal,
  removable and what is mounted, then refuses when the disk is a partition, holds the running system (macOS:
  `/`'s whole disk and its APFS physical stores; Linux: the disks under `/`, `/boot`, `/boot/efi`, `/usr`,
  `/home`, following dm/LVM slaves, and swap), is internal, is not on USB, is a disk image / virtual, reports
  no size, is larger than **256 GiB** (every stick sold as 256 GB passes, 512 GB and SSDs do not), or (Linux)
  has mounted file systems. Dry run by default (prints the commands); `--real` asks you to **type the device
  name** (`disk4`), re-reads the disk and refuses if size/model/bus changed, then: macOS `diskutil
  unmountDisk`, `sudo dd ... of=/dev/rdiskN bs=1m`, read back + SHA-256, `diskutil eject`; Linux `sudo dd
  ... conv=fsync`, read back + SHA-256.
- Built for this commit (Lima, 2026-11-05): `build/installer/rescue/starbite-rescue-usb.img`, 64 MiB,
  sha256 `33c7eaec2b4c4241cbb17e7b4641a23438ce79e3c5940c214c15ad78b135cd87` (MBR type 0x0c at 2048,
  FAT32 label `SB-RESCUE`). The image has a build time in its README, so a rebuild has another hash.
- The phone side needs the store entries `rescue.image`, `rescue.dtb`, `rescue.initrd` (in the bundle's UFS
  store) and a loader with the rescue path (docs/STARBITE-RECOVERY.md, docs/RESCUE-USB-HOST.md).

## 8. Tests and the rehearsal

- Unit tests (`tools/installer/tests/`, 59 tests; `rehearse.sh`: 63 checks): disk safety with recorded diskutil plists (internal
  disk, APFS container, USB stick, 1 TB USB SSD, disk image, internal SD reader, 256 GB stick) and fake
  sysfs/proc trees (SATA root disk, USB stick, USB SSD, mounted stick, stick under an LVM root); the
  destination allowlist, manifest checks, bundle verification, symlink escape, hash cache, `--update-hashes`;
  the TOML fallback parser against tomllib; the ext4 reader and the no-password check on real mke2fs images
  (`tests/fixtures/*.ext4.gz`, `tests/mk-ext4-fixtures.sh`); the journal, chunk state and the chunk writer
  resuming after a simulated failure; plan classification; MBR round trip.
- `tools/installer/rehearse.sh`: the real flow end to end against fake phones (`fakes/fakephone.py`: the
  main phone's GPT layout at its real block numbers and sizes as sparse files, Samsung's small partitions
  filled with random bytes, a sparse 128 GB card) through the fake `adb`, `fastboot` and `heimdall`, which run
  TWRP's shell commands on the host with the phone's paths redirected. The existing tools (part-backup.sh,
  part-write.py, slot-write.py, store-flash*.sh, card-range-flash.py, metadata-reset.sh) run unchanged
  against it. Fake limitations: partition nodes are separate files (not windows into one disk), `[ -b` is
  read as `[ -e`, and `mke2fs`, `blkid`, `mount`, `mkfs.exfat`, `getprop`, `blockdev` are shell stand-ins.

Transcript of `RESCUE_IMG=build/installer/rescue/starbite-rescue-usb.img tools/installer/rehearse.sh`
(2026-11-05, macOS; long runs shown as headers, plan rows and ok/WARN/REFUSED/STOPPED lines):

```

######## 1. a rehearsal bundle (fake images; real OS stores and ext4 roots) and a fake phone (blank 128 GB card)
rehearsal bundle in build/installer-rehearsal/bundle
fake phone in <repo>/build/installer-rehearsal/phone: state recovery, card blank (249999360 sectors)

######## 2. the bundle against its manifest
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
PASS bundle verified (exit 0)
PASS 14 items present, 2 optional ones absent

######## 3. refusals before anything is written
REFUSED: the bundle does not match its manifest:
PASS a missing file is refused (exit 2)
PASS names the missing file
REFUSED: the bundle does not match its manifest:
PASS a changed file is refused (exit 2)
PASS names the file whose hash differs
REFUSED: destination part:EFS: EFS is a protected area; the installer never writes it
PASS a manifest that names EFS is refused (exit 2)
PASS says EFS is protected
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: recovery (adb)
REFUSED: no finished dry run of this bundle on phone-5c7ed06646: run the same command with --dry-run first (it writes nothing), then again
PASS --rehearse before a dry run is refused (exit 2)
PASS asks for the dry run first
REFUSED: --yes is not accepted with --real: every confirmation is typed by a person
PASS --yes with --real is refused (exit 2)
REFUSED: --fake-root and --real do not mix
PASS --fake-root with --real is refused (exit 2)

######## 4. dry run (fresh install: --replace-all): reads the phone, prints every command, writes nothing
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: recovery (adb)
== The plan ==
  1  host  Back up the protected areas  this computer
  2  verify  Check the UFS layout (GPT)  -
  3  write  microSD partition table (p1 exFAT, p2 raw)  microSD sector 0
  4  write  Format microSD p1 (exFAT)  microSD p1
  5  REPLACE  iOS 16.5 (an earlier port) root: APFS system + HFSX /var  UFS USERDATA bytes 0.. (Apple slot, below 16 GiB)  2.0 MiB  tools/refport/slot-write.py
  6  write  OS store (UFS): catalog, kernels, payloads, Engineer OS, rescue system  UFS OS store  1.2 MiB  tools/store/store-flash.sh
  7  write  Android 17 system (Google GSI)  UFS partition SYSTEM  129.0 MiB  tools/android/pixel/part-write.py
  8  write  Android vendor (Starbite build)  UFS partition VENDOR  3.0 MiB  tools/android/pixel/part-write.py
  9  REPLACE  Android /data (empty f2fs)  UFS partition A16DATA  4.0 MiB  tools/android/pixel/part-write.py
  10  REPLACE  Android /metadata (fresh ext4)  UFS partition ODM  tools/android/pixel/metadata-reset.sh
  11  REPLACE  Windows 11 boot files (ESP)  UFS partition WINESP  2.0 MiB  installer chunk writer
  12  skip  Windows 11 (NTFS)  UFS partition WINDOWS  installer chunk writer
  13  write  OS store (microSD p2)  microSD p2 OS store  92.0 KiB  tools/store/store-flash-sd.sh
  14  write  macOS Ventura root (HFSX, 15 GiB)  microSD sectors 216442880..  3.0 MiB  tools/macos/card-range-flash.py
  15  skip  iOS 26 root (parked edge pair)  microSD sectors 182888448..  tools/macos/card-range-flash.py
  16  write  postmarketOS (Phosh)  microSD p1 (exFAT) /distros/pmos.img  8.0 MiB  installer chunk writer (file)
  17  write  Ubuntu (Phosh)  microSD p1 (exFAT) /distros/ubuntu.img  8.0 MiB  installer chunk writer (file)
  18  write  ChromiumOS (GPT disk image)  microSD p1 (exFAT) /distros/chromeos.img  2.0 MiB  installer chunk writer (file)
  19  write  TWRP (Starbite: /data = A16DATA)  UFS partition RECOVERY  5.0 MiB  installer chunk writer
  20  write  Starbite loader + menu (BOOT)  UFS partition BOOT  6.0 MiB  installer chunk writer
  21  verify  No user password on any OS  -
  22  verify  Finish: rescue USB offer, reboot into Starbite  -
WARN iOS 26 (ios26) will not boot from this bundle: root card:lba:182888448
WARN Windows 11 (win11) will not boot from this bundle: root part:WINDOWS
WARN password: Linux /distros/ubuntu.img: user "user" has a PIN: rebuild the image with SB_NO_PIN=1
WARN password: macOS Ventura: auto-login, but the account HAS a password ("starbite"): asked only if WindowServer restarts; an empty password is untested on the phone
ok  the UFS layout matched in preflight
  $ python3 -I tools/android/pixel/part-write.py SYSTEM build/installer-rehearsal/bundle/android/system.img --state build/installer-rehearsal/state/phone-5c7ed06646/chunks/android-system-3dde938c7282.state  [dry run: not run]
  $ python3 -I tools/android/pixel/part-write.py VENDOR build/installer-rehearsal/bundle/android/vendor.img --state build/installer-rehearsal/state/phone-5c7ed06646/chunks/android-vendor-9fb377de6d23.state  [dry run: not run]
  $ python3 -I tools/android/pixel/part-write.py A16DATA build/installer-rehearsal/bundle/android/a16data.img --state build/installer-rehearsal/state/phone-5c7ed06646/chunks/android-data-091662c8d6a3.state  [dry run: not run]
ok  Android 17: by-design: empty f2fs from make-a16data.sh + fresh ODM: no lock credential exists
ok  Linux /distros/pmos.img: ok: user "user": empty password; /etc/starbite/login = none (lock screen off)
WARN Linux /distros/ubuntu.img: OPEN: user "user" has a PIN: rebuild the image with SB_NO_PIN=1
ok  ChromiumOS: by-design: no account is created; Guest needs no password
ok  Windows 11: ok: build-image.ps1 unattend: empty password + AutoLogon
WARN macOS Ventura: OPEN: auto-login, but the account HAS a password ("starbite"): asked only if WindowServer restarts; an empty password is untested on the phone
ok  iOS 16.5: by-design: no passcode until one is set in Settings
ok  Engineer OS / Starbite: ok: no login; armed mode is hold-and-slide, not a PIN
  written 0, skipped 2, kept 0; 0 write commands ran
WARN iOS 26 (ios26) will not boot from this bundle: root card:lba:182888448
WARN Windows 11 (win11) will not boot from this bundle: root part:WINDOWS
WARN password: Linux /distros/ubuntu.img: user "user" has a PIN: rebuild the image with SB_NO_PIN=1
WARN password: macOS Ventura: auto-login, but the account HAS a password ("starbite"): asked only if WindowServer restarts; an empty password is untested on the phone
== Dry run finished ==
ok  nothing was written (26 write commands printed). The real mode (--real, or --rehearse on a fake phone) is now allowed for this bundle on this phone.
PASS dry run (exit 0)
PASS dry run: the fake phone is byte-identical
PASS dry run printed its write commands
PASS exact command for SYSTEM shown

######## 5. rehearsal with a cable pull in the middle of SYSTEM (chunk 1 of 3)
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: recovery (adb)
== The plan ==
  1  host  Back up the protected areas  this computer
  2  verify  Check the UFS layout (GPT)  -
  3  write  microSD partition table (p1 exFAT, p2 raw)  microSD sector 0
  4  write  Format microSD p1 (exFAT)  microSD p1
  5  REPLACE  iOS 16.5 (an earlier port) root: APFS system + HFSX /var  UFS USERDATA bytes 0.. (Apple slot, below 16 GiB)  2.0 MiB  tools/refport/slot-write.py
  6  write  OS store (UFS): catalog, kernels, payloads, Engineer OS, rescue system  UFS OS store  1.2 MiB  tools/store/store-flash.sh
  7  write  Android 17 system (Google GSI)  UFS partition SYSTEM  129.0 MiB  tools/android/pixel/part-write.py
  8  write  Android vendor (Starbite build)  UFS partition VENDOR  3.0 MiB  tools/android/pixel/part-write.py
  9  REPLACE  Android /data (empty f2fs)  UFS partition A16DATA  4.0 MiB  tools/android/pixel/part-write.py
  10  REPLACE  Android /metadata (fresh ext4)  UFS partition ODM  tools/android/pixel/metadata-reset.sh
  11  REPLACE  Windows 11 boot files (ESP)  UFS partition WINESP  2.0 MiB  installer chunk writer
  12  skip  Windows 11 (NTFS)  UFS partition WINDOWS  installer chunk writer
  13  write  OS store (microSD p2)  microSD p2 OS store  92.0 KiB  tools/store/store-flash-sd.sh
  14  write  macOS Ventura root (HFSX, 15 GiB)  microSD sectors 216442880..  3.0 MiB  tools/macos/card-range-flash.py
  15  skip  iOS 26 root (parked edge pair)  microSD sectors 182888448..  tools/macos/card-range-flash.py
  16  write  postmarketOS (Phosh)  microSD p1 (exFAT) /distros/pmos.img  8.0 MiB  installer chunk writer (file)
  17  write  Ubuntu (Phosh)  microSD p1 (exFAT) /distros/ubuntu.img  8.0 MiB  installer chunk writer (file)
  18  write  ChromiumOS (GPT disk image)  microSD p1 (exFAT) /distros/chromeos.img  2.0 MiB  installer chunk writer (file)
  19  write  TWRP (Starbite: /data = A16DATA)  UFS partition RECOVERY  5.0 MiB  installer chunk writer
  20  write  Starbite loader + menu (BOOT)  UFS partition BOOT  6.0 MiB  installer chunk writer
  21  verify  No user password on any OS  -
  22  verify  Finish: rescue USB offer, reboot into Starbite  -
ok  sda-head-4M: 4.0 MiB, sha256 53c9b224b4b5792f... (phone and local agree)
ok  sda-tail-gpt: 20.0 KiB, sha256 21772b9f4fac83f2... (phone and local agree)
ok  sdb: 4.0 MiB, sha256 3dd5959e5c0017e7... (phone and local agree)
ok  sdc: 4.0 MiB, sha256 f886219db1e6bda7... (phone and local agree)
ok  sdd: 2.0 MiB, sha256 67fd4a3943b40aa3... (phone and local agree)
ok  sde: 2.0 MiB, sha256 9917235ed8fe362d... (phone and local agree)
ok  backup complete: 24 items in build/installer-rehearsal/state/phone-5c7ed06646/backups/20261105-232519
ok  the UFS layout matched in preflight
ok  card MBR written and read back; partitions re-read
ok  p1 formatted exFAT "STARBITE"
ok  ios16-slot: verified on the phone (read back)
ok  ufs-store: verified on the phone (read back)
STOPPED: command failed (1): python3 -I tools/android/pixel/part-write.py SYSTEM build/installer-rehearsal/bundle/android/system.img --state build/installer-rehearsal/state/phone-5c7ed06646/chunks/android-system-3dde938c7282.state
PASS stopped by the simulated cable pull (exit 3)
PASS the failure is reported as STOPPED (resumable)
PASS SYSTEM chunk state kept: 0 

######## 6. the same command again: resumes
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: recovery (adb)
== The plan ==
  1  skip  Back up the protected areas  this computer
  2  verify  Check the UFS layout (GPT)  -
  3  skip  microSD partition table (p1 exFAT, p2 raw)  microSD sector 0
  4  skip  Format microSD p1 (exFAT)  microSD p1
  5  keep  iOS 16.5 (an earlier port) root: APFS system + HFSX /var  UFS USERDATA bytes 0.. (Apple slot, below 16 GiB)  2.0 MiB  tools/refport/slot-write.py
  6  skip  OS store (UFS): catalog, kernels, payloads, Engineer OS, rescue system  UFS OS store  1.2 MiB  tools/store/store-flash.sh
  7  resume  Android 17 system (Google GSI)  UFS partition SYSTEM  129.0 MiB  tools/android/pixel/part-write.py
  8  write  Android vendor (Starbite build)  UFS partition VENDOR  3.0 MiB  tools/android/pixel/part-write.py
  9  REPLACE  Android /data (empty f2fs)  UFS partition A16DATA  4.0 MiB  tools/android/pixel/part-write.py
  10  REPLACE  Android /metadata (fresh ext4)  UFS partition ODM  tools/android/pixel/metadata-reset.sh
  11  REPLACE  Windows 11 boot files (ESP)  UFS partition WINESP  2.0 MiB  installer chunk writer
  12  skip  Windows 11 (NTFS)  UFS partition WINDOWS  installer chunk writer
  13  write  OS store (microSD p2)  microSD p2 OS store  92.0 KiB  tools/store/store-flash-sd.sh
  14  write  macOS Ventura root (HFSX, 15 GiB)  microSD sectors 216442880..  3.0 MiB  tools/macos/card-range-flash.py
  15  skip  iOS 26 root (parked edge pair)  microSD sectors 182888448..  tools/macos/card-range-flash.py
  16  write  postmarketOS (Phosh)  microSD p1 (exFAT) /distros/pmos.img  8.0 MiB  installer chunk writer (file)
  17  write  Ubuntu (Phosh)  microSD p1 (exFAT) /distros/ubuntu.img  8.0 MiB  installer chunk writer (file)
  18  write  ChromiumOS (GPT disk image)  microSD p1 (exFAT) /distros/chromeos.img  2.0 MiB  installer chunk writer (file)
  19  write  TWRP (Starbite: /data = A16DATA)  UFS partition RECOVERY  5.0 MiB  installer chunk writer
  20  write  Starbite loader + menu (BOOT)  UFS partition BOOT  6.0 MiB  installer chunk writer
  21  verify  No user password on any OS  -
  22  verify  Finish: rescue USB offer, reboot into Starbite  -
ok  the UFS layout matched in preflight
ok  android-system: verified on the phone (read back)
ok  android-vendor: verified on the phone (read back)
ok  android-data: verified on the phone (read back)
ok  android-metadata: verified on the phone (read back)
ok  winesp: verified on the phone (read back)
ok  sd-store: verified on the phone (read back)
ok  mac13-root: verified on the phone (read back)
ok  pmos: verified on the phone (read back)
ok  ubuntu: verified on the phone (read back)
ok  chromeos: verified on the phone (read back)
ok  twrp: verified on the phone (read back)
ok  boot: verified on the phone (read back)
ok  Android 17: ok: A16DATA and ODM written fresh by this install: no lock credential exists
ok  Linux /distros/pmos.img: ok: user "user": empty password; /etc/starbite/login = none (lock screen off)
ok  ChromiumOS: by-design: no account is created; Guest needs no password
ok  Windows 11: ok: build-image.ps1 unattend: empty password + AutoLogon
ok  iOS 16.5: by-design: no passcode until one is set in Settings
ok  Engineer OS / Starbite: ok: no login; armed mode is hold-and-slide, not a PIN
  written 12, skipped 6, kept 1; 25 write commands ran
PASS resumed run finished (exit 0)
PASS SYSTEM resumed (not restarted)
PASS the finished backup is not taken again

######## 7. and again: nothing left to do
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: recovery (adb)
== The plan ==
  1  skip  Back up the protected areas  this computer
  2  verify  Check the UFS layout (GPT)  -
  3  skip  microSD partition table (p1 exFAT, p2 raw)  microSD sector 0
  4  skip  Format microSD p1 (exFAT)  microSD p1
  5  keep  iOS 16.5 (an earlier port) root: APFS system + HFSX /var  UFS USERDATA bytes 0.. (Apple slot, below 16 GiB)  2.0 MiB  tools/refport/slot-write.py
  6  skip  OS store (UFS): catalog, kernels, payloads, Engineer OS, rescue system  UFS OS store  1.2 MiB  tools/store/store-flash.sh
  7  skip  Android 17 system (Google GSI)  UFS partition SYSTEM  129.0 MiB  tools/android/pixel/part-write.py
  8  skip  Android vendor (Starbite build)  UFS partition VENDOR  3.0 MiB  tools/android/pixel/part-write.py
  9  keep  Android /data (empty f2fs)  UFS partition A16DATA  4.0 MiB  tools/android/pixel/part-write.py
  10  keep  Android /metadata (fresh ext4)  UFS partition ODM  tools/android/pixel/metadata-reset.sh
  11  keep  Windows 11 boot files (ESP)  UFS partition WINESP  2.0 MiB  installer chunk writer
  12  skip  Windows 11 (NTFS)  UFS partition WINDOWS  installer chunk writer
  13  skip  OS store (microSD p2)  microSD p2 OS store  92.0 KiB  tools/store/store-flash-sd.sh
  14  keep  macOS Ventura root (HFSX, 15 GiB)  microSD sectors 216442880..  3.0 MiB  tools/macos/card-range-flash.py
  15  skip  iOS 26 root (parked edge pair)  microSD sectors 182888448..  tools/macos/card-range-flash.py
  16  keep  postmarketOS (Phosh)  microSD p1 (exFAT) /distros/pmos.img  8.0 MiB  installer chunk writer (file)
  17  keep  Ubuntu (Phosh)  microSD p1 (exFAT) /distros/ubuntu.img  8.0 MiB  installer chunk writer (file)
  18  keep  ChromiumOS (GPT disk image)  microSD p1 (exFAT) /distros/chromeos.img  2.0 MiB  installer chunk writer (file)
  19  skip  TWRP (Starbite: /data = A16DATA)  UFS partition RECOVERY  5.0 MiB  installer chunk writer
  20  skip  Starbite loader + menu (BOOT)  UFS partition BOOT  6.0 MiB  installer chunk writer
  21  verify  No user password on any OS  -
  22  verify  Finish: rescue USB offer, reboot into Starbite  -
ok  the UFS layout matched in preflight
ok  Android 17: ok: A16DATA and ODM written fresh by this install: no lock credential exists
ok  Linux /distros/pmos.img: ok: user "user": empty password; /etc/starbite/login = none (lock screen off)
ok  ChromiumOS: by-design: no account is created; Guest needs no password
ok  Windows 11: ok: build-image.ps1 unattend: empty password + AutoLogon
ok  iOS 16.5: by-design: no passcode until one is set in Settings
ok  Engineer OS / Starbite: ok: no login; armed mode is hold-and-slide, not a PIN
  written 0, skipped 11, kept 8; 0 write commands ran
PASS third run (exit 0)
PASS 0 write commands

######## 8. checks on the fake phone
PASS EFS, CPEFS, PARAM, UP_PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA0-2, DTB, DTBO, GPT head/tail, sdb-sde: byte-identical
  boot             part:BOOT                        ok
  twrp             part:RECOVERY                    ok
  ios16-slot       slot:USERDATA@0                  ok
  android-system   part:SYSTEM                      ok
  android-vendor   part:VENDOR                      ok
  android-data     part:A16DATA                     ok
  winesp           part:WINESP                      ok
  mac13-root       card:lba:216442880               ok
  pmos             card:file:/distros/pmos.img      ok
  ubuntu           card:file:/distros/ubuntu.img    ok
  chromeos         card:file:/distros/chromeos.img  ok
PASS every written target reads back as its image
PASS backup: 24 areas, each hashed on the phone and on this computer
  android-data=done, android-metadata=done, android-system=done, android-vendor=done, boot=done, chromeos=done, ios16-slot=done, mac13-root=done, pmos=done, sd-store=done, twrp=done, ubuntu=done, ufs-store=done, winesp=done
PASS journal: every present item done, ODM and A16DATA fresh

######## 9. the main phone without a journal (installed some other way): nothing replaced
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: recovery (adb)
== The plan ==
  1  host  Back up the protected areas  this computer
  2  verify  Check the UFS layout (GPT)  -
  3  skip  microSD partition table (p1 exFAT, p2 raw)  microSD sector 0
  4  skip  Format microSD p1 (exFAT)  microSD p1
  5  keep  iOS 16.5 (an earlier port) root: APFS system + HFSX /var  UFS USERDATA bytes 0.. (Apple slot, below 16 GiB)  2.0 MiB  tools/refport/slot-write.py
  6  skip  OS store (UFS): catalog, kernels, payloads, Engineer OS, rescue system  UFS OS store  1.2 MiB  tools/store/store-flash.sh
  7  skip  Android 17 system (Google GSI)  UFS partition SYSTEM  129.0 MiB  tools/android/pixel/part-write.py
  8  skip  Android vendor (Starbite build)  UFS partition VENDOR  3.0 MiB  tools/android/pixel/part-write.py
  9  keep  Android /data (empty f2fs)  UFS partition A16DATA  4.0 MiB  tools/android/pixel/part-write.py
  10  keep  Android /metadata (fresh ext4)  UFS partition ODM  tools/android/pixel/metadata-reset.sh
  11  keep  Windows 11 boot files (ESP)  UFS partition WINESP  2.0 MiB  installer chunk writer
  12  skip  Windows 11 (NTFS)  UFS partition WINDOWS  installer chunk writer
  13  skip  OS store (microSD p2)  microSD p2 OS store  92.0 KiB  tools/store/store-flash-sd.sh
  14  keep  macOS Ventura root (HFSX, 15 GiB)  microSD sectors 216442880..  3.0 MiB  tools/macos/card-range-flash.py
  15  skip  iOS 26 root (parked edge pair)  microSD sectors 182888448..  tools/macos/card-range-flash.py
  16  keep  postmarketOS (Phosh)  microSD p1 (exFAT) /distros/pmos.img  8.0 MiB  installer chunk writer (file)
  17  keep  Ubuntu (Phosh)  microSD p1 (exFAT) /distros/ubuntu.img  8.0 MiB  installer chunk writer (file)
  18  keep  ChromiumOS (GPT disk image)  microSD p1 (exFAT) /distros/chromeos.img  2.0 MiB  installer chunk writer (file)
  19  skip  TWRP (Starbite: /data = A16DATA)  UFS partition RECOVERY  5.0 MiB  installer chunk writer
  20  skip  Starbite loader + menu (BOOT)  UFS partition BOOT  6.0 MiB  installer chunk writer
  21  verify  No user password on any OS  -
  22  verify  Finish: rescue USB offer, reboot into Starbite  -
ok  the UFS layout matched in preflight
ok  Linux /distros/pmos.img: ok: user "user": empty password; /etc/starbite/login = none (lock screen off)
ok  ChromiumOS: by-design: no account is created; Guest needs no password
ok  Windows 11: ok: build-image.ps1 unattend: empty password + AutoLogon
ok  iOS 16.5: by-design: no passcode until one is set in Settings
ok  Engineer OS / Starbite: ok: no login; armed mode is hold-and-slide, not a PIN
  written 0, skipped 10, kept 8; 0 write commands ran
== Dry run finished ==
ok  nothing was written (18 write commands printed). The real mode (--real, or --rehearse on a fake phone) is now allowed for this bundle on this phone.
PASS dry run with a fresh state directory (exit 0)
PASS data on the card is kept
PASS Android data is kept
PASS identical BOOT is skipped
PASS says how to replace on purpose

######## 10. a phone in Download mode (no TWRP yet): TWRP via heimdall, then the dry run gate
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: download (Samsung Download mode (04e8:685d))
  $ heimdall flash --RECOVERY <repo>/build/installer-rehearsal/bundle/boot/twrp-starbite.img --no-reboot  [dry run: not run]
== The plan (no phone: targets not checked) ==
ok  offline dry run: nothing was read from or written to a phone
PASS dry run in Download mode: shows the heimdall step, plans without phone checks (exit 0)
PASS the exact heimdall command
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: download (Samsung Download mode (04e8:685d))
  $ heimdall flash --RECOVERY <repo>/build/installer-rehearsal/bundle/boot/twrp-starbite.img --no-reboot
ok  TWRP is up
REFUSED: no finished dry run of this bundle on phone-5c7ed06646: run the same command with --dry-run first (it writes nothing), then again
PASS rehearsal: TWRP flashed and booted, then the gate asks for a dry run (exit 2)
PASS TWRP came up after the (fake) key combination
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: recovery (adb)
== The plan ==
  1  host  Back up the protected areas  this computer
  2  verify  Check the UFS layout (GPT)  -
  3  skip  microSD partition table (p1 exFAT, p2 raw)  microSD sector 0
  4  skip  Format microSD p1 (exFAT)  microSD p1
  5  REPLACE  iOS 16.5 (an earlier port) root: APFS system + HFSX /var  UFS USERDATA bytes 0.. (Apple slot, below 16 GiB)  2.0 MiB  tools/refport/slot-write.py
  6  write  OS store (UFS): catalog, kernels, payloads, Engineer OS, rescue system  UFS OS store  1.2 MiB  tools/store/store-flash.sh
  7  write  Android 17 system (Google GSI)  UFS partition SYSTEM  129.0 MiB  tools/android/pixel/part-write.py
  8  write  Android vendor (Starbite build)  UFS partition VENDOR  3.0 MiB  tools/android/pixel/part-write.py
  9  REPLACE  Android /data (empty f2fs)  UFS partition A16DATA  4.0 MiB  tools/android/pixel/part-write.py
  10  REPLACE  Android /metadata (fresh ext4)  UFS partition ODM  tools/android/pixel/metadata-reset.sh
  11  REPLACE  Windows 11 boot files (ESP)  UFS partition WINESP  2.0 MiB  installer chunk writer
  12  skip  Windows 11 (NTFS)  UFS partition WINDOWS  installer chunk writer
  13  write  OS store (microSD p2)  microSD p2 OS store  92.0 KiB  tools/store/store-flash-sd.sh
  14  write  macOS Ventura root (HFSX, 15 GiB)  microSD sectors 216442880..  3.0 MiB  tools/macos/card-range-flash.py
  15  skip  iOS 26 root (parked edge pair)  microSD sectors 182888448..  tools/macos/card-range-flash.py
  16  write  postmarketOS (Phosh)  microSD p1 (exFAT) /distros/pmos.img  8.0 MiB  installer chunk writer (file)
  17  write  Ubuntu (Phosh)  microSD p1 (exFAT) /distros/ubuntu.img  8.0 MiB  installer chunk writer (file)
  18  write  ChromiumOS (GPT disk image)  microSD p1 (exFAT) /distros/chromeos.img  2.0 MiB  installer chunk writer (file)
  19  skip  TWRP (Starbite: /data = A16DATA)  UFS partition RECOVERY  5.0 MiB  installer chunk writer
  20  write  Starbite loader + menu (BOOT)  UFS partition BOOT  6.0 MiB  installer chunk writer
  21  verify  No user password on any OS  -
  22  verify  Finish: rescue USB offer, reboot into Starbite  -
ok  the UFS layout matched in preflight
  $ python3 -I tools/android/pixel/part-write.py SYSTEM build/installer-rehearsal/bundle/android/system.img --state build/installer-rehearsal/state-dl/phone-5c7ed06646/chunks/android-system-3dde938c7282.state  [dry run: not run]
  $ python3 -I tools/android/pixel/part-write.py VENDOR build/installer-rehearsal/bundle/android/vendor.img --state build/installer-rehearsal/state-dl/phone-5c7ed06646/chunks/android-vendor-9fb377de6d23.state  [dry run: not run]
  $ python3 -I tools/android/pixel/part-write.py A16DATA build/installer-rehearsal/bundle/android/a16data.img --state build/installer-rehearsal/state-dl/phone-5c7ed06646/chunks/android-data-091662c8d6a3.state  [dry run: not run]
ok  Android 17: by-design: empty f2fs from make-a16data.sh + fresh ODM: no lock credential exists
ok  Linux /distros/pmos.img: ok: user "user": empty password; /etc/starbite/login = none (lock screen off)
ok  ChromiumOS: by-design: no account is created; Guest needs no password
ok  Windows 11: ok: build-image.ps1 unattend: empty password + AutoLogon
ok  iOS 16.5: by-design: no passcode until one is set in Settings
ok  Engineer OS / Starbite: ok: no login; armed mode is hold-and-slide, not a PIN
  written 0, skipped 5, kept 0; 0 write commands ran
== Dry run finished ==
ok  nothing was written (26 write commands printed). The real mode (--real, or --rehearse on a fake phone) is now allowed for this bundle on this phone.
PASS dry run in TWRP (exit 0)
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: recovery (adb)
== The plan ==
  1  host  Back up the protected areas  this computer
  2  verify  Check the UFS layout (GPT)  -
  3  skip  microSD partition table (p1 exFAT, p2 raw)  microSD sector 0
  4  skip  Format microSD p1 (exFAT)  microSD p1
  5  REPLACE  iOS 16.5 (an earlier port) root: APFS system + HFSX /var  UFS USERDATA bytes 0.. (Apple slot, below 16 GiB)  2.0 MiB  tools/refport/slot-write.py
  6  write  OS store (UFS): catalog, kernels, payloads, Engineer OS, rescue system  UFS OS store  1.2 MiB  tools/store/store-flash.sh
  7  write  Android 17 system (Google GSI)  UFS partition SYSTEM  129.0 MiB  tools/android/pixel/part-write.py
  8  write  Android vendor (Starbite build)  UFS partition VENDOR  3.0 MiB  tools/android/pixel/part-write.py
  9  REPLACE  Android /data (empty f2fs)  UFS partition A16DATA  4.0 MiB  tools/android/pixel/part-write.py
  10  REPLACE  Android /metadata (fresh ext4)  UFS partition ODM  tools/android/pixel/metadata-reset.sh
  11  REPLACE  Windows 11 boot files (ESP)  UFS partition WINESP  2.0 MiB  installer chunk writer
  12  skip  Windows 11 (NTFS)  UFS partition WINDOWS  installer chunk writer
  13  write  OS store (microSD p2)  microSD p2 OS store  92.0 KiB  tools/store/store-flash-sd.sh
  14  write  macOS Ventura root (HFSX, 15 GiB)  microSD sectors 216442880..  3.0 MiB  tools/macos/card-range-flash.py
  15  skip  iOS 26 root (parked edge pair)  microSD sectors 182888448..  tools/macos/card-range-flash.py
  16  write  postmarketOS (Phosh)  microSD p1 (exFAT) /distros/pmos.img  8.0 MiB  installer chunk writer (file)
  17  write  Ubuntu (Phosh)  microSD p1 (exFAT) /distros/ubuntu.img  8.0 MiB  installer chunk writer (file)
  18  write  ChromiumOS (GPT disk image)  microSD p1 (exFAT) /distros/chromeos.img  2.0 MiB  installer chunk writer (file)
  19  skip  TWRP (Starbite: /data = A16DATA)  UFS partition RECOVERY  5.0 MiB  installer chunk writer
  20  write  Starbite loader + menu (BOOT)  UFS partition BOOT  6.0 MiB  installer chunk writer
  21  verify  No user password on any OS  -
  22  verify  Finish: rescue USB offer, reboot into Starbite  -
ok  sda-head-4M: 4.0 MiB, sha256 3025fbae603453ac... (phone and local agree)
ok  sda-tail-gpt: 20.0 KiB, sha256 fed08c9d9e2d437c... (phone and local agree)
ok  sdb: 4.0 MiB, sha256 e5815dbb0d35fe94... (phone and local agree)
ok  sdc: 4.0 MiB, sha256 03419076353c197c... (phone and local agree)
ok  sdd: 2.0 MiB, sha256 e3ffcda24d03f837... (phone and local agree)
ok  sde: 2.0 MiB, sha256 94677fab286be011... (phone and local agree)
ok  backup complete: 24 items in build/installer-rehearsal/state-dl/phone-5c7ed06646/backups/20261105-232539
ok  the UFS layout matched in preflight
ok  ios16-slot: verified on the phone (read back)
ok  ufs-store: verified on the phone (read back)
ok  android-system: verified on the phone (read back)
ok  android-vendor: verified on the phone (read back)
ok  android-data: verified on the phone (read back)
ok  android-metadata: verified on the phone (read back)
ok  winesp: verified on the phone (read back)
ok  sd-store: verified on the phone (read back)
ok  mac13-root: verified on the phone (read back)
ok  pmos: verified on the phone (read back)
ok  ubuntu: verified on the phone (read back)
ok  chromeos: verified on the phone (read back)
ok  boot: verified on the phone (read back)
ok  Android 17: ok: A16DATA and ODM written fresh by this install: no lock credential exists
ok  Linux /distros/pmos.img: ok: user "user": empty password; /etc/starbite/login = none (lock screen off)
ok  ChromiumOS: by-design: no account is created; Guest needs no password
ok  Windows 11: ok: build-image.ps1 unattend: empty password + AutoLogon
ok  iOS 16.5: by-design: no passcode until one is set in Settings
ok  Engineer OS / Starbite: ok: no login; armed mode is hold-and-slide, not a PIN
  written 13, skipped 5, kept 0; 42 write commands ran
PASS rehearsal completes (exit 0)
PASS installed

######## 11. a phone in the Starbite loader's fastboot
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: fastboot-starbite (starbite-fb1)
  $ fastboot reboot recovery  [dry run: not run]
== The plan (no phone: targets not checked) ==
ok  offline dry run: nothing was read from or written to a phone
PASS dry run: no reboot in a dry run (exit 0)
PASS shows fastboot reboot recovery
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: fastboot-starbite (starbite-fb1)
ok  TWRP is up
REFUSED: no finished dry run of this bundle on phone-5c7ed06646: run the same command with --dry-run first (it writes nothing), then again
PASS rehearsal: reboots to TWRP, then the gate (exit 2)
PASS detected the Starbite loader's fastboot

######## 12. preflight refusals: another phone model, a 32 GB card, a flat battery, a card with another layout
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: recovery (adb)
REFUSED: preflight failed:
PASS an S9+ (SM-G965F) is refused (exit 2)
PASS names the model
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: recovery (adb)
REFUSED: preflight failed:
PASS a 32 GB card and 20 % battery are refused (exit 2)
PASS the card is too small
PASS the battery is too low
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: recovery (adb)
== The plan ==
  1  host  Back up the protected areas  this computer
  2  verify  Check the UFS layout (GPT)  -
  3  BLOCKED  microSD partition table (p1 exFAT, p2 raw)  microSD sector 0
  4  BLOCKED  Format microSD p1 (exFAT)  microSD p1
  5  keep  iOS 16.5 (an earlier port) root: APFS system + HFSX /var  UFS USERDATA bytes 0.. (Apple slot, below 16 GiB)  2.0 MiB  tools/refport/slot-write.py
  6  write  OS store (UFS): catalog, kernels, payloads, Engineer OS, rescue system  UFS OS store  1.2 MiB  tools/store/store-flash.sh
  7  write  Android 17 system (Google GSI)  UFS partition SYSTEM  129.0 MiB  tools/android/pixel/part-write.py
  8  write  Android vendor (Starbite build)  UFS partition VENDOR  3.0 MiB  tools/android/pixel/part-write.py
  9  keep  Android /data (empty f2fs)  UFS partition A16DATA  4.0 MiB  tools/android/pixel/part-write.py
  10  keep  Android /metadata (fresh ext4)  UFS partition ODM  tools/android/pixel/metadata-reset.sh
  11  keep  Windows 11 boot files (ESP)  UFS partition WINESP  2.0 MiB  installer chunk writer
  12  skip  Windows 11 (NTFS)  UFS partition WINDOWS  installer chunk writer
  13  write  OS store (microSD p2)  microSD p2 OS store  92.0 KiB  tools/store/store-flash-sd.sh
  14  write  macOS Ventura root (HFSX, 15 GiB)  microSD sectors 216442880..  3.0 MiB  tools/macos/card-range-flash.py
  15  skip  iOS 26 root (parked edge pair)  microSD sectors 182888448..  tools/macos/card-range-flash.py
  16  write  postmarketOS (Phosh)  microSD p1 (exFAT) /distros/pmos.img  8.0 MiB  installer chunk writer (file)
  17  write  Ubuntu (Phosh)  microSD p1 (exFAT) /distros/ubuntu.img  8.0 MiB  installer chunk writer (file)
  18  write  ChromiumOS (GPT disk image)  microSD p1 (exFAT) /distros/chromeos.img  2.0 MiB  installer chunk writer (file)
  19  write  TWRP (Starbite: /data = A16DATA)  UFS partition RECOVERY  5.0 MiB  installer chunk writer
  20  write  Starbite loader + menu (BOOT)  UFS partition BOOT  6.0 MiB  installer chunk writer
  21  verify  No user password on any OS  -
  22  verify  Finish: rescue USB offer, reboot into Starbite  -
REFUSED: the plan has blocked steps: microSD partition table (p1 exFAT, p2 raw): the card has another layout (p1 type 0x0c 2048+249997312); everything on it would be lost: run again with --erase-card; Format microSD p1 (exFAT): p1 is not exFAT: run again with --erase-card to format it
PASS a card with another layout is not erased without --erase-card (exit 2)
PASS the card step is BLOCKED
ok  bundle verified: 14 items; optional items not in it: windows, ios26-root
  found: recovery (adb)
== The plan ==
  1  host  Back up the protected areas  this computer
  2  verify  Check the UFS layout (GPT)  -
  3  REPLACE  microSD partition table (p1 exFAT, p2 raw)  microSD sector 0
  4  write  Format microSD p1 (exFAT)  microSD p1
  5  keep  iOS 16.5 (an earlier port) root: APFS system + HFSX /var  UFS USERDATA bytes 0.. (Apple slot, below 16 GiB)  2.0 MiB  tools/refport/slot-write.py
  6  write  OS store (UFS): catalog, kernels, payloads, Engineer OS, rescue system  UFS OS store  1.2 MiB  tools/store/store-flash.sh
  7  write  Android 17 system (Google GSI)  UFS partition SYSTEM  129.0 MiB  tools/android/pixel/part-write.py
  8  write  Android vendor (Starbite build)  UFS partition VENDOR  3.0 MiB  tools/android/pixel/part-write.py
  9  keep  Android /data (empty f2fs)  UFS partition A16DATA  4.0 MiB  tools/android/pixel/part-write.py
  10  keep  Android /metadata (fresh ext4)  UFS partition ODM  tools/android/pixel/metadata-reset.sh
  11  keep  Windows 11 boot files (ESP)  UFS partition WINESP  2.0 MiB  installer chunk writer
  12  skip  Windows 11 (NTFS)  UFS partition WINDOWS  installer chunk writer
  13  write  OS store (microSD p2)  microSD p2 OS store  92.0 KiB  tools/store/store-flash-sd.sh
  14  write  macOS Ventura root (HFSX, 15 GiB)  microSD sectors 216442880..  3.0 MiB  tools/macos/card-range-flash.py
  15  skip  iOS 26 root (parked edge pair)  microSD sectors 182888448..  tools/macos/card-range-flash.py
  16  write  postmarketOS (Phosh)  microSD p1 (exFAT) /distros/pmos.img  8.0 MiB  installer chunk writer (file)
  17  write  Ubuntu (Phosh)  microSD p1 (exFAT) /distros/ubuntu.img  8.0 MiB  installer chunk writer (file)
  18  write  ChromiumOS (GPT disk image)  microSD p1 (exFAT) /distros/chromeos.img  2.0 MiB  installer chunk writer (file)
  19  write  TWRP (Starbite: /data = A16DATA)  UFS partition RECOVERY  5.0 MiB  installer chunk writer
  20  write  Starbite loader + menu (BOOT)  UFS partition BOOT  6.0 MiB  installer chunk writer
  21  verify  No user password on any OS  -
  22  verify  Finish: rescue USB offer, reboot into Starbite  -
ok  the UFS layout matched in preflight
  $ python3 -I tools/android/pixel/part-write.py SYSTEM build/installer-rehearsal/bundle/android/system.img --state build/installer-rehearsal/state-x/phone-5c7ed06646/chunks/android-system-3dde938c7282.state  [dry run: not run]
  $ python3 -I tools/android/pixel/part-write.py VENDOR build/installer-rehearsal/bundle/android/vendor.img --state build/installer-rehearsal/state-x/phone-5c7ed06646/chunks/android-vendor-9fb377de6d23.state  [dry run: not run]
ok  Linux /distros/pmos.img: ok: user "user": empty password; /etc/starbite/login = none (lock screen off)
ok  ChromiumOS: by-design: no account is created; Guest needs no password
ok  Windows 11: ok: build-image.ps1 unattend: empty password + AutoLogon
ok  iOS 16.5: by-design: no passcode until one is set in Settings
ok  Engineer OS / Starbite: ok: no login; armed mode is hold-and-slide, not a PIN
  written 0, skipped 2, kept 4; 0 write commands ran
== Dry run finished ==
ok  nothing was written (23 write commands printed). The real mode (--real, or --rehearse on a fake phone) is now allowed for this bundle on this phone.
PASS with --erase-card the plan erases it (exit 0)
PASS the card step is REPLACE

######## 13. rescue USB stick (fake disks; no real disk)
rescue image: starbite-rescue-usb.img
REFUSED: /dev/disk0 will not be written: it holds the running system; it is an internal disk; it is not on USB (bus: Apple Fabric)
PASS the Mac's internal disk is refused (exit 2)
PASS ...because it holds the running system
REFUSED: /dev/disk5 will not be written: it is larger than 256 GiB (931.5 GiB): not a rescue stick
PASS a 1 TB USB SSD is refused (exit 2)
PASS ...because it is larger than 256 GiB
REFUSED: /dev/disk6 will not be written: it is not on USB (bus: Disk Image); it is a virtual disk / disk image
PASS a disk image is refused (exit 2)
REFUSED: /dev/disk4s1: give a whole USB disk as /dev/sdX (from `lsblk -d -o NAME,TRAN,SIZE,MODEL`)
PASS a partition name is refused (exit 2)
  $ diskutil unmountDisk /dev/disk4  (prepare)
  $ sudo dd if=build/installer-rehearsal/rescue.img of=/dev/rdisk4 bs=1m  (write)
  $ diskutil eject /dev/disk4  (finish)
ok  dry run: nothing written. Run again with --real to write the stick.
PASS dry run on the 32 GB stick shows the disk and the commands (exit 0)
PASS the exact dd command (raw device)
  $ diskutil unmountDisk /dev/disk4  (prepare)
  $ sudo dd if=build/installer-rehearsal/rescue.img of=/dev/rdisk4 bs=1m  (write)
  $ diskutil eject /dev/disk4  (finish)
ok  /dev/disk4: rescue disk written and read back (sha256 33c7eaec2b4c4241...). Label SB-RESCUE.
PASS rehearsal: typed name, written, read back (exit 0)
PASS the fake stick starts with the image

######## result: 63 passed, 0 failed
```

## 9. Repartitioning (not in v1)

A stock S9 has Samsung's GPT: USERDATA spans the space where Starbite has USERDATA, WINESP, WINDOWS and
A16DATA. v1 refuses such a phone in preflight ("the GPT is not the Starbite layout this bundle was made for").
The split was made once on S9 #1 with `tools/woa/gpt-split.sh` (offline GPT build in Lima with sgdisk, apply
from TWRP with read-back), and the 16 GiB A16DATA variant is `tools/android/pixel/gpt-a16grow.py`. Folding a
GPT step into the installer means writing a protected area, which the rules allow only behind a verified
backup of that phone's own copy and never in the default flow: it should come as a separate, explicitly
confirmed subcommand that uses the backup step's `sda-head-4M.img` / `sda-tail-gpt.img` as its input,
writes entries 25+ only, keeps entries 1-24, the protective MBR and the disk GUID byte-identical, reads both
copies back and re-reads the partition table. That is the next piece of work, and it should be rehearsed
on a disk image first.

## 10. Implemented vs stubbed, and what needs the real phone

**Implemented and rehearsed (fakes):** the CLI and screens; the two confirmations; state detection (adb,
fastboot with `version-bootloader`, Download mode by USB ID via `ioreg` / sysfs); getting to TWRP from
Android, the loader's fastboot and Download mode; preflight; the plan; dry run; the dry-run gate; typed
confirmation; backup; GPT check; card MBR + exFAT; every writer; read-back verification; journal, skip /
keep / resume / replace; A16DATA+ODM pairing; no-password checks; rescue image build and stick write with
the disk-safety rules; the summary.

**Stubbed or not done:**
- GPT repartitioning of a stock phone (section 9): refused, not written.
- No real bundle exists (section 5); the example manifest has placeholder sizes and hashes.
- Card layouts other than the 128 GB card's: the card roots' sectors are in payload command lines and
  `card-range-flash.py` accepts only the 249,999,360-sector card, so a 64 GB card needs its own bundle
  (other `sdroot=` sectors, rebuilt payloads) and that tool taught other sizes.
- macOS Ventura still has a password (section 6).
- Windows: no clean image; the WINDOWS write path exists but has only run against the fake.
- UP_PARAM (the Starbite splash) and the bootloader (VG6) step of the master plan are not part of v1.
- The rescue stick carries the sample script only (no backups, no restore script yet).
- The settings record (default entry) is the phone's own (store-flash keeps it) or the loader's defaults.

**Needs the real phone to verify:**
1. TWRP's `getprop` names (`ro.product.model`, `ro.product.device`, `ro.boot.bootloader`, `ro.boot.serialno`,
   `ro.boot.flash.locked`, `ro.boot.verifiedbootstate`, `ro.twrp.version`) and `/sys/class/power_supply/battery/capacity`.
2. That one `adb shell` fact script runs in TWRP's shell (toybox/busybox: `readlink -f`, `command -v`, `df -k`).
3. `fastboot devices` / `getvar version-bootloader` against the real loader, and `fastboot reboot recovery` landing in TWRP.
4. Download mode detection by USB ID 04e8:685d on the Mac (`ioreg`), and `heimdall flash --RECOVERY ... --no-reboot` followed by the key combination.
5. TWRP's exFAT formatter: whether `mkfs.exfat` or `mkexfatfs` exists, and that `-n STARBITE` is its label flag.
6. Mounting p1 at `/external_sd` (`mount -t exfat` or `exfat-fuse`), `blockdev --rereadpt /dev/block/mmcblk0` with nothing mounted, and the new `mmcblk0p1/p2` nodes.
7. Unmounting what TWRP mounts on its own (`/data` = A16DATA in the Starbite TWRP) before a write.
8. Speed: the chunk writer and the read-back hashes over adb for multi-GiB items (WINDOWS, card roots: the card reads at about 20 MB/s, so a 15 GiB verify takes ~13 min).
9. The backup of the boot LUNs `sdb`-`sde` (names and sizes on this UFS) and of `sda`'s tail.
10. The final `adb reboot` landing in the Starbite menu.
11. On a phone with a fresh install: Android's setup wizard without a lock, the Linux images built with `SB_NO_PIN=1` (empty password accepted by PAM, Phosh's lock screen really off), Windows signing in by itself.
12. The rescue stick on the phone (the whole of docs/RESCUE-USB-HOST.md section 5).
13. `--make-rescue-usb --real` on a real stick (the coordinator does this after review).
