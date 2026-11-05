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
| `[card]` | `min_bytes` (at least 60,000,000,000: a card sold as 64 GB holds 61.9-64.1 GB), `layout` (`"sd128"` S9 #1's 128 GB card, `"sd64"` any card of at least 64 GB: section 11; p1, p2 and the root sectors then come from `tools/store/cardlayout.py`, and `--verify-bundle` checks the catalog roots and the payloads' `sdroot=` against it), `sectors` (the exact card this layout was made for; card roots sit at fixed sectors in payload command lines; not with `sd64`), `p1`/`p2` `{type, start, sectors[, label]}`: the MBR the card layout step writes (with `layout`: may be left out, must agree if given) |
| `[[layout]]` | `name`, `node`, `start`, `blocks` (4 KiB blocks): the UFS GPT the set was made for. Checked in preflight, never written |
| `[[item]]` | one thing to install, below |

| Item key | Meaning |
|---|---|
| `key` | unique, `[a-z0-9-]`; `--replace KEY` names it |
| `title`, `os`, `note` | shown; `os` (starbite, android, linux, chromeos, windows, macos, ios) drives the no-password list |
| `file`, `size`, `sha256` | path inside the bundle (no `..`, no symlink out), byte size, SHA-256. `--update-hashes` fills the last two |
| `dest` | `part:NAME` (BOOT RECOVERY SYSTEM VENDOR A16DATA WINESP WINDOWS; ODM only with `action = "mkfs-metadata"`), `slot:USERDATA@OFFSET` (the 16 GiB Apple slot), `store:ufs`, `store:sd`, `card:lba:SECTOR` (inside `[card] p2`, 1 MiB aligned, no overlaps; with a card layout inside one of its root slots), `card:root:KEY` (with `[card] layout`: the root slot of KEY, e.g. `card:root:mac13`, resolved to `card:lba:` at load), `card:file:/PATH` (a file on card p1's exFAT) |
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

- Unit tests (`tools/installer/tests/`, 77 tests, 18 of them the card layouts of section 11;
  `rehearse.sh`: 87 checks, 24 of them the 64 GB card of section 11.6; the transcript below is the
  128 GB part, which still reads the same apart from an `ok card layout sd128: ...` line per run): disk safety with recorded diskutil plists (internal
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
  `dd` onto a `/dev/block` stand-in gets `conv=notrunc` (a block device cannot be truncated; before that,
  `card-range-flash.py`'s `dd` cut the fake card off after the macOS root, unnoticed on the 128 GB card
  because nothing lies behind that root, and fatal on `sd64`, where p1 does).

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
- Card layouts: done on the host for any card of at least 64 GB (section 11, layout `sd64`). Still open
  there: the XNU kernel's clock and kernel-log sectors (a kernel rebuild, 11.5), and everything in 11.7
  that needs the phone. iOS 26 (parked) has no slot on `sd64`.
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

## 11. microSD card layouts: the 128 GB card and any card of 64 GB or more

Written 2026-11-06. The user's spec says the phone needs "an SD card of at least 64 GB". Until now every
card number was S9 #1's 128 GB card (249,999,360 sectors), spelled out in a dozen places. Now the geometry
lives in one module, `tools/store/cardlayout.py`, with two layouts:

- **`sd128`**: S9 #1's card exactly as it is. Nothing on it moves; every number below is the one the phone
  has today.
- **`sd64`**: any card of at least 60,000,000,000 bytes (117,187,500 sectors: every card sold as 64 GB, and
  any bigger one). The raw p2 comes first and has a fixed size, so the macOS root is at the same sector
  on every card and one set of payloads serves every card. p1 exFAT takes the rest of the card.

`python3 tools/store/cardlayout.py show [sd128|sd64] [--sectors N]` prints a layout for a given card.

### 11.1 Inventory: every place that assumes the card's geometry

"From the table" means the value now comes from `cardlayout.py` (the same value as before for `sd128`).

**Host tools**

| Where | What it assumes | Status |
|---|---|---|
| `tools/macos/card-range-flash.py:31-32`, `:63` | card = 249,999,360 sectors; test range 249868288+131072 | from the table; without `--layout` unchanged (128 GB card only); `--layout sd128\|sd64` (`:52`, `:66-92`) checks the card size, that the MBR is the layout's, and that the range is in a root slot |
| `tools/macos/card-range-flash-fast.py:26-28`, `:48`, `:59-60` | the same, plus the iOS 26 range 182888448+33554432 | not changed: iOS 26 root-cycle tool, `sd128` only |
| `tools/macos/root26-cycle.py:29`, `:40`; `root26-pull.py:11`; `ventura-harvest.py:13`; `card-root-compare.py:7` | root starts 216442880 (Ventura) and 182888448 (iOS 26) | not changed: development tools for S9 #1's card |
| `tools/macos/build-macos-root.sh:10` (comment) | the 64 MiB test range at 249868288 | not changed |
| `tools/store/sbstore2.py:54-58` (`MEDIA['sd']` 8389120, `SD_ROOT_TAIL_BLOCKS` 16384), `:251-257`, `:277-284` | card store capacity = the 128 GB card's p2; the tail = p2's last 64 MiB | from the table; new `--medium sd64` (capacity 3,964,928 blocks; refuses a store larger than the 64 MiB store area); `header` also reports the store area |
| `tools/store/store-flash-sd.sh:13-21`, `:30` | p2 is MBR entry 2 (`mmcblk0p2`), type 0xDA; store capacity == p2 blocks | generic already (read from the card's MBR): unchanged |
| `tools/store/boot-next.sh:29-34` | clock "SBTIME01" in sector 249999359, only on a 249,999,360-sector card | not changed (11.5) |
| `tools/store/boot-next.sh:51-57`, `:77` | the sdlog ring at sector 249868288 | not changed (11.5) |
| `tools/store/catalog.toml:38`, `:48`, `:284` | roots `sd:lba 216442880+31457280` (mac13), `182888448+33554432` (ios26, ios26sdt) | `cardlayout.py catalog LAYOUT IN OUT` writes them for a layout (identity on `sd128`); the installer checks them |
| `build/macos13-user.txt` (untracked) packed into `out/xnu-mac13-v33.xnu` at offset 0x48 (`tools/macos/mac13-payload.sh:10`, `tools/mkpayload-ios26.sh:15`, `:28-30`) | `sdroot=lba:216442880,31457280` in the Ventura payload's boot-args (and the iOS 26 payloads' `182888448,33554432`) | `cardlayout.py payload LAYOUT KEY IN OUT` rewrites it. Checked on the real v33 payload (in memory, nothing written): byte-identical for `sd128`; for `sd64` only the cmdline field differs (`sdroot=lba:139264,31457280`, 514 of 607 bytes) |
| `tools/cmdline/macos13-t0.txt`, `-t0b.txt`, `ios26-sdprobe.txt` (`sdroot=lba:249868288,131072`), `macos13-t1.txt` (`241348608,7987200`), `macos26-demo/-m1/-t1.txt` (`182888448,20971520`) | test and parked command lines | not changed; `cardlayout.py cmdline LAYOUT KEY FILE` retargets one when needed |
| `tools/installer/installer-manifest.example.toml:22-27`, `:187`, `:197` | `[card] sectors/p1/p2`, `card:lba:216442880`, `card:lba:182888448` | now also `layout = "sd128"`; the values are unchanged and checked against the table |
| `tools/installer/sbinst/common.py:32` | `MIN_CARD_BYTES` 60,000,000,000 | from the table |
| `tools/installer/sbinst/steps.py:207-223` | "card layout fit" = the exact card size | with `[card] layout`: that layout's rule (exact / at least) and which layout the card needs |
| `tools/installer/sbinst/steps.py:374-417`, `:600-652` | MBR and exFAT format from the manifest's fixed p1/p2 | from the layout, sized for the card in the phone; another Starbite layout on the card is named in the BLOCKED reason |
| `tools/installer/fakes/fakephone.py:35-36` | the 128 GB card's p1/p2 | from the table; `--layout sd64` lays a fake card out as `sd64` |
| `tools/installer/tests/test_manifest.py:18-19`, `:31`, `:132`, `:140`; `test_resume.py:170`, `:200-201` | 128 GB card values in fixtures | unchanged (they test manifests without a layout) |
| `tools/linux/init:35` (`SDDEV=/dev/mmcblk0p1`), `tools/linux/distros/card-install.sh:42-46` | p1 is MBR entry 1 and exFAT; no sector numbers | holds on both layouts (p1 stays entry 1 on `sd64`) |
| `tools/android/fstab.card-a16` | the old card-slot Android (p5-p9 logical partitions) | not in the lineup or the installer; not changed |

**Loader and Engineer OS**: no fixed sectors anywhere.

| Where | What it assumes |
|---|---|
| `shim/starlte/sd.c:424-443` `sd_store_area` (same in `starbite/libsb/sd.c:427-446`) | the card store is the first primary MBR entry of type 0xDA with a 4 KiB-aligned start and at least 64 sectors. Position and size come from the MBR, so `sd64`'s p2 at sector 8192 works unchanged |
| `shim/starlte/store.c:146-152` | the card store's capacity fits that entry (`open_v2` limit) |
| `shim/starlte/em.c:2589`, `starbite/engineer/pages_em.c:1966` | read the MBR for display only |

**XNU kernel** (`xnu/series-12377`, in the Ventura and iOS 26 kernels)

| Where | What it assumes | On `sd64` |
|---|---|---|
| `0345-starlte-microsd-driver.patch:893-971` (the `sdroot=` parser) | `sdroot=lba:START,COUNT` or `pN` from the boot-args; checked against the CSD's card size (`:909`, `:924`, `:938`); refuses FAT/exFAT/NTFS overlap and a start below the 0xDA store's used end (`:844`) | generic: works unchanged |
| `0370-starlte-sdlog.patch:58`, `:65`; `0377-starlte-sdlog-busy-card.patch:28` | the kernel log ring (`sdlog=1`) at 249868288+131072, only on a 249,999,360-sector card | silently off: no kernel log on the card |
| `0384-starlte-sd-dirty-chunks.patch` | the written-chunk bitmap, relative to the `sdroot=` range, saved in the sdlog header | generic, but rides on the sdlog header (off) |
| `0393-starlte-sd-clock.patch:29`, `:46`, `:51`; `0395-starlte-sd-clock-periodic.patch:37`, `:54` | the clock "SBTIME01" in sector 249999359, only on a 249,999,360-sector card | silently off: macOS starts at 1970 ("System clock is incorrect") |
| `0383-starlte-msgbuf-8m.patch:20` (comment) | the 64 MiB card log region | - |

### 11.2 What each OS needs on the card, and what fits on 64 GB

Only these keep anything on the card. iOS 16.5, Android 17, Windows 11/10 and the Starbite OSes live on
UFS (the card store holds only a fallback copy of Engineer OS).

| Item | Where | Size | Source |
|---|---|---|---|
| card OS store (catalog, Engineer OS fallback of 10-16 MB) | p2 head | `sd128`: a 1 MiB area (today's card stores are 32-92 KiB); `sd64`: a 64 MiB area | `build/store/sd-store-min.img`; docs/STARBITE-MASTER-PLAN.md 2.5 |
| macOS Ventura (active stable pair) | p2 raw range | 15 GiB (31,457,280 sectors) | catalog.toml:38, `build/macos13-user.txt` |
| iOS 26 (parked edge pair) | p2 raw range | 16 GiB (33,554,432 sectors) | catalog.toml:48 |
| tail: XNU test range, sdlog ring, clock | p2's last 64 MiB | 64 MiB | starlte_sd.c (11.1) |
| postmarketOS | p1 `/distros/pmos.img` | 8 GiB budget (2,143 MiB today) | MASTER-PLAN 2.4, 2.5 |
| Ubuntu (Phosh) | p1 `/distros/ubuntu.img` | 8 GiB budget [est] (built in 8 GiB, then shrunk) | `tools/linux/distros/mkroot-ubuntu.sh:28` |
| ChromiumOS | p1 `/distros/chromeos.img` | 6.34 GiB (6.8 GB; its GPT ends at sector 13,295,615) | docs/CHROMEOS-STATUS.md:15, :23 |
| Ubuntu Touch (in the lineup, not built yet) | p1 `/distros/ut.img` | 8 GiB [est] | MASTER-PLAN 2.4 |
| p1 working room: tools, staging, slack | p1 | 2 + 4 + 0.5 GiB | MASTER-PLAN 2.4 (p1 budget) |
| MBR and alignment | sectors 0..8191 | 4 MiB | |

Against the smallest card the installer accepts (60.0 GB = 55.88 GiB; a SanDisk "64 GB" card has
124,735,488 sectors = 59.48 GiB):

| Set | Size | Fits in 55.88 GiB? |
|---|---|---|
| today's bundle with iOS 26: store + Ventura + iOS 26 + tail + pmOS + Ubuntu + ChromiumOS + lead-in | 53.47 GiB | only just: 2.4 GiB left for files, staging and growth |
| the same plus Ubuntu Touch (the lineup's Linux pair) | 61.47 GiB | **no** |
| the master plan's full p1 budget (36 GiB) plus store, Ventura, iOS 26 and tail | 67.13 GiB | **no** |
| **`sd64`**: store + Ventura + tail (15.13 GiB) + the p1 budget (36 GiB) | 51.13 GiB | **yes**: p1 is 40.75 GiB on the smallest card (44.35 GiB on the SanDisk card), 4.75 GiB above the budget |

So the 64 GB layout carries every OS of the lineup that lives on the card **except iOS 26**, which is
parked (the edge pair) and keeps its root only on S9 #1's 128 GB card. Priority if space runs short:
1. the card store and the tail (tiny: the card fallback, the kernel log and the clock), 2. macOS Ventura
(the active stable pair), 3. postmarketOS, 4. Ubuntu / Ubuntu Touch, 5. ChromiumOS (later real ChromeOS),
6. staging and the user's files, 7. iOS 26 (128 GB card only).

### 11.3 The `sd64` layout

```
sector 0             MBR (entry 1 = p1 exFAT, entry 2 = p2 0xDA, as on sd128)
8192..139263         p2: card OS store area, 64 MiB (sbstore2.py --medium sd64: a 3,964,928-block store,
                         at most 16,384 blocks used)
139264..31596543         macOS Ventura root, 15 GiB  ->  sdroot=lba:139264,31457280
31596544..31727615       tail, 64 MiB (test range / sdlog ring; 31727615 = the clock sector)
31727616..           p1: exFAT "STARBITE" to the card's last whole MiB (85,458,944 sectors = 40.75 GiB on
                         a 60.0 GB card, 93,007,872 on a SanDisk 64 GB, 218,271,744 on a 128 GB card)
```

Why this shape:
- **Fixed sectors for everything the boot side names.** The roots are in payload boot-args, and the card
  store's capacity is in its header (store-flash-sd.sh refuses a store whose capacity is not p2's size).
  With p2 first and of fixed size, one bundle (one catalog, one Ventura payload, one card store image)
  fits every card of 64 GB or more; a bigger card only gets a bigger p1. A layout computed per card size
  would need the payloads and the store rebuilt for each card, after the bundle's hashes were made.
- **MBR entry numbers unchanged.** p1 (exFAT) is still entry 1, so `/dev/block/mmcblk0p1` in TWRP, the
  Linux initrd (`SDDEV=/dev/mmcblk0p1`), card-install.sh and Android's vold see the same thing; p2 is
  entry 2, the loader's first 0xDA entry and store-flash-sd.sh's `mmcblk0p2`. Only its position differs.
- **One tail rule for both layouts: the last 64 MiB of the 0xDA partition.** On `sd128` p2 ends at the
  card's end, so that is today's 249868288 / 249999359; sbstore2.py's tail check already said "the last
  16384 blocks of p2". The kernel change in 11.5 uses exactly this rule.
- exFAT no longer starts at the card's beginning (where SD controllers expect the FAT); p1 is 4 MiB
  aligned as on `sd128`. Worth a speed check on the phone (11.7).
- Any card that is not S9 #1's exact card gets `sd64`, including a 128 GB card of another brand (then
  without iOS 26). A "big card" variant could add an iOS 26 slot behind the macOS root at a fixed sector;
  not made, since the edge pair is parked.

### 11.4 What was implemented (host side)

- `tools/store/cardlayout.py`: the table (`sd128`, `sd64`); card fit and identification (`for_card`,
  `identify`); boot-args and SWEETXNU payload retargeting (`retarget_cmdline`, `retarget_payload`: only the
  608-byte cmdline at offset 0x48 changes; no checksum covers it, and the store's CRC is made when the
  store is built); catalog roots for a layout (`catalog_text`: entries without a slot are hidden and their
  root says so; keys are kept, so sbstore2.py `--import` still accepts it); a CLI (`show`, `for-card`,
  `manifest`, `cmdline`, `payload`, `payload-root`, `catalog`). Standard library only.
- `tools/store/sbstore2.py`: `MEDIA` and the tail from the table; `--medium sd64` with its store-area check;
  `header` reports the store area. `--medium sd` builds exactly as before.
- `tools/macos/card-range-flash.py`: constants from the table; `--layout sd128|sd64`. Without it unchanged.
- Installer: `[card] layout` and `card:root:KEY`, with root-slot checks at load; `--verify-bundle` (and every
  run) refuses a bundle whose catalog roots or payload `sdroot=` do not match its layout
  (`manifest.layout_problems`); preflight checks the card against the layout and names the layout a card
  needs; the MBR and exFAT steps use the layout's p1/p2 for the card in the phone; a card with the other
  Starbite layout is BLOCKED (both named) unless `--erase-card`; `card-range-flash.py` gets `--layout sd64`
  (never for `sd128`, whose command line stays the one it always was); the plan prints the layout and its
  `PENDING` items (11.5); `--describe-phone` names the card's layout.
- Fakes: `fakephone.py create --card starbite --layout sd64`; `mkbundle.py OUT --layout sd64` builds the
  bundle the way a real bundle builder would (below); the fake `adb`'s `dd` no longer truncates
  block-device stand-ins (section 8).

Making a real `sd64` bundle from the `sd128` parts (no rebuild):

```sh
python3 tools/store/cardlayout.py catalog sd64 tools/store/catalog.toml BUNDLE/store/catalog.toml
python3 tools/store/cardlayout.py payload sd64 mac13 out/xnu-mac13-v33.xnu out/xnu-mac13-v33-sd64.xnu
python3 tools/store/sbstore2.py build BUNDLE/store/ufs-store.img --medium ufs --catalog BUNDLE/store/catalog.toml \
    --import build/store/ufs-store-mac13-v33.img --omit ios26-sd.xnu --omit ios26-sdt.xnu \
    mac13-v33.xnu=out/xnu-mac13-v33-sd64.xnu:xnu ...
python3 tools/store/sbstore2.py build BUNDLE/store/sd-store.img --medium sd64 engineer.img=...
# installer-manifest.toml: [card] layout = "sd64" (no sectors/p1/p2); mac13-root dest = "card:root:mac13";
# no ios26-root item. Then --update-hashes and --verify-bundle, which checks all of the above.
```

The macOS root image is the same file on both layouts (HFSX does not know where it sits).

### 11.5 What needs a rebuild (not done)

**The XNU kernel's clock and log sectors.** On `sd64` today's kernel boots macOS Ventura from
`sdroot=lba:139264,31457280` (the driver is generic), but `sdlog=1` and the SBTIME01 clock are tied to the
249,999,360-sector card. So on `sd64` there is no clock from the card (macOS starts at 1970) and no kernel
log in the tail. The change, as one new patch after 0395 in `xnu/series-12377` (`bsd/dev/starlte_sd.c`):

1. Once the card is up (`sd_up_locked`, after the CSD), read sector 0 and take the first primary MBR entry
   of type 0xDA with a 4 KiB-aligned start and at least 131,072 + 8 sectors (the loader's `sd_store_area`
   rule): `sd_tail_first = start + count - 131072`, `sd_tail_end = start + count`; both 0 when there is none.
2. 0370/0377 `starlte_sd_dump_log`: `sd.card_sectors != 249999360ULL` becomes `!sd_tail_end`, and
   `first = 249868288ULL` becomes `first = sd_tail_first`.
3. 0393: `SD_TIME_SECTOR` becomes `sd_tail_end - 1` (both uses), the guard `sd.card_sectors != 249999360ULL`
   becomes `!sd_tail_end`; 0395: both `sd.card_sectors == 249999360ULL` become `sd_tail_end != 0`.

On `sd128` that gives 249868288 and 249999359, today's sectors: same bytes, same behaviour. Then:
`tools/build-xnu-12377.sh` (Mac, Xcode 16.4, KDK 26.5), the Ventura kernel that `tools/macos/mac13-payload.sh:10`
reads (`out/kernel.development.starlte-12377-ventura`), a new `mac13-vN` payload from `build/macos13-user.txt`,
and for `sd64` `cardlayout.py payload sd64 mac13` on it. The host side follows in `tools/store/boot-next.sh`:
the clock at the layout's `clock_sector` and the log at its `tail.start`, for the layout `identify` finds on
the card. Until that kernel exists this would only write a sector nobody reads, so it waits for it. The plan
screen lists all of this as `PENDING` for `sd64` (from `cardlayout.py`'s `pending`).

Nothing else needs a rebuild: the loader, Engineer OS, the Linux kernel and initrd, and the iOS 16.5 path
have no card sectors in them.

**Found on the way (`sd128`, not changed):** the 128 GB card's store area is only 1 MiB (256 blocks)
before the iOS 26 root, but `sbstore2.py --medium sd` refuses only at the tail. A card store above 1 MiB
(say with the Engineer OS fallback copy) would be written over the head of the iOS 26 root by
store-flash-sd.sh. `sbstore2.py header` now prints `sd128 store area ...: OVERLAPPED by N blocks` for such
a store; making `--medium sd` refuse it is a one-line change, left for the user to approve.

### 11.6 Tests and the rehearsal

- `tools/installer/tests/test_cardlayout.py` (18 tests): the layout rules (alignment, the areas tile p2,
  store first, tail last, clock sector); `sd128` equal to every constant still spelled out elsewhere
  (sbstore2.py, card-range-flash.py, fakephone.py, boot-next.sh, the XNU patches 0370/0393/0395, the
  catalog roots, the example manifest); `sd64` on 60 GB, 64 GB, 128 GB and 256 GB cards; refusals (a 32 GB
  card, one sector short, `sd128` on another card); `for_card` and `identify`; cmdline and payload
  retargeting (identity on `sd128`, only the cmdline field changes, refusals); catalog conversion (identity
  on `sd128`, iOS 26 hidden, packs with sbstore2.py); `sbstore2.py --medium sd64`;
  `card-range-flash.py --check-only` through the fake adb on small sparse fake cards (default path,
  `--layout sd64`, tail/store/p1 ranges refused, an `sd128` MBR under `--layout sd64` named, no MBR, a
  32 GB card); manifest `layout` / `card:root:` and their refusals; `layout_problems` for matching and
  mismatched payloads and catalogs.
- `tools/installer/rehearse.sh` section 14 (24 checks): an `sd64` bundle on a fake SanDisk 64 GB card is
  verified; the `sd128` bundle is refused on it (and says which layout the card needs); an `sd64` bundle
  carrying the 128 GB card's payloads is refused; the dry run writes nothing; the rehearsal writes
  everything; the fake card then has the `sd64` MBR, a 3,964,928-block store, the macOS root at 139264 and
  the Linux files; a second run writes nothing; an `sd64` bundle on a card with S9 #1's layout is BLOCKED
  with both layout names, and laid out with `--erase-card` (same p2, p1 218,271,744 sectors).
- Run 2026-11-06 (macOS): unit tests 77/77 OK; `rehearse.sh` 87 passed, 0 failed. Sections 1-13 (the 128 GB
  card) are the same 63 checks as before, all passing.

Section 14 of the run (shortened):

```
######## 14. a 64 GB card: the sd64 card layout (tools/store/cardlayout.py; docs/INSTALLER.md 11)
rehearsal bundle in build/installer-rehearsal/bundle64 (sd64 card layout)
ok  card layout sd64: catalog roots and payload sdroot= match (mac13 139264)
ok  bundle verified: 14 items; optional items not in it: windows
REFUSED: preflight failed:
PASS the 128 GB card's bundle (sd128) on a 64 GB card is refused (exit 2)
PASS ...because sd128 is S9 #1's card only
PASS ...and says which layout this card needs
REFUSED: the bundle does not match its card layout (sd64):
PASS an sd64 bundle carrying the 128 GB card's payloads is refused (exit 2)
PASS names the payload and both sectors
  3  write  microSD partition table (p1 exFAT, p2 raw)  microSD sector 0
  14  write  macOS Ventura root (HFSX, 15 GiB)  microSD sectors 139264..  3.0 MiB  tools/macos/card-range-flash.py
WARN card layout sd64: PENDING: XNU kernel: the SBTIME01 clock and the sdlog ring are read/written only on ...
PASS sd64 dry run: the fake phone is byte-identical
PASS p2 first (fixed), p1 exFAT to the card's end
PASS card-range-flash.py is called with --layout sd64
ok  card MBR written and read back; partitions re-read
ok  mac13-root: verified on the phone (read back)
  written 14, skipped 1, kept 0; 51 write commands ran
  MBR: p1 (7, 31727616, 93007872), p2 (218, 8192, 31719424): sd64
PASS sd64 card: MBR, store capacity, macOS root and Linux files as the layout says
  written 0, skipped 10, kept 8; 0 write commands ran
REFUSED: the plan has blocked steps: microSD partition table (p1 exFAT, p2 raw): the card has the Starbite
  sd128 card layout and this bundle is made for sd64 (p1 type 0x07 8192+182878208, p2 type 0xda ...); ...
PASS ...with --erase-card the 128 GB card is laid out as sd64 (exit 0)
PASS sd64 on a 128 GB card: same p2, a bigger p1
######## result: 87 passed, 0 failed
```

### 11.7 What needs the real phone (and a 64 GB card)

Nothing here touched S9 #1 or its card, and `sd128` is unchanged, so S9 #1 needs nothing. To prove
`sd64`, with a spare card of 64 GB or more in the phone (the 128 GB card set aside, backed up first):
1. TWRP mounting p1 when p1 is not at the card's start (`mount -t exfat /dev/block/mmcblk0p1`), and
   `blockdev --rereadpt` picking up the `sd64` MBR.
2. The loader's card store at sector 8192 (Engineer > Storage > Card store at boot), and Engineer OS from it.
3. macOS Ventura from `sdroot=lba:139264,31457280` with today's kernel: the driver's "past the store's
   used end" and "no exFAT overlap" checks at the new positions, boot to the desktop; the clock stays at
   1970 until the 11.5 kernel.
4. The Linux distros and ChromiumOS from p1 (initrd `SDDEV=/dev/mmcblk0p1`), and Android 17's vold
   showing only p1, with no format offer.
5. exFAT speed on p1 away from the card's start (SD controllers tune their first allocation units for FAT).
6. After 11.5: the clock and the sdlog ring in the `sd64` tail, and `boot-next.sh` reading them.
