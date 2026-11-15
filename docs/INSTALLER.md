# Starbite Installer (v2)

Written 2026-11-05. Item 7 of `docs/STARBITE-2-PLAN.md` "Current order": from a Mac or a Linux PC you run
one script; it asks you to confirm an unlocked bootloader and a microSD of at least 64 GB, then makes the
phone match the main Starbite S9, OS by OS, every write verified by reading it back. No user password or
passcode on any OS (section 6). Rescue USB disks for the Bixby > Starbite Recovery > Rescue mode path are
made by the same script (section 7).

**v2 (2026-11-06): a stock SM-G960F (stock firmware, stock GPT, bootloader unlocked) + a microSD of at
least 64 GB becomes a Starbite phone with one script: Download mode -> the Starbite loader into BOOT ->
the loader's fastboot starts the Starbite installer environment from RAM -> backup -> the UFS repartitioned
to the table in `sbinst/ufslayout.py` (section 9) -> card -> every OS. No TWRP anywhere: the installer never
writes or boots RECOVERY (section 13). Bundles are made by `--build-bundle` (section 12).**

**Status: host side done and rehearsed against fake phones and fake disks. Nothing has run against the
real phone. The installer environment is now Starbite Maintenance in installer mode (docs/MAINTENANCE.md):
built 2026-11-06 (`tools/installer/env/mk-env.sh` -> `tools/maint/env/mk-maint.sh`), rehearsed in the VM.
Starbite Maintenance itself has run on S9 #1, started from the OS store (2026-11-06: USB ECM, ssh, by-name,
`build/maint/logs/20261106-*`); the loader's `fastboot boot` of a Linux kernel (how the installer starts it)
has not. First real install: S9 #2, 2026-11-15, section 15.**
Section 10 lists exactly what needs the phone.

## 1. Quick start

```sh
tools/installer/starbite-install --bundle BUNDLE                 # = --dry-run: reads the phone, writes nothing
tools/installer/starbite-install --bundle BUNDLE --real          # after a finished dry run; typed confirmation
tools/installer/starbite-install --verify-bundle BUNDLE          # files vs manifest (size + SHA-256)
tools/installer/starbite-install --update-hashes BUNDLE          # whoever builds a bundle: fill size + sha256
tools/installer/starbite-install --bundle BUNDLE --real --repartition   # a stock phone: its GPT is re-laid (section 9)
tools/installer/starbite-install --bundle BUNDLE --start-env      # only start the installer environment
tools/installer/starbite-install --restore-gpt [--real]          # the phone's own GPT back (saved before repartitioning)
tools/installer/starbite-install --build-bundle DIR --from-phone [--card-layout sd64]   # section 12
tools/installer/starbite-install --describe-phone                # the phone's layout as manifest TOML (read only)
python3 tools/installer/sbinst/ufslayout.py show [LAYOUT]        # the UFS tables (section 9.1)
python3 tools/installer/sbinst/gpt.py show|plan HEAD TAIL [LAYOUT]   # the GPT engine on a backup
tools/installer/starbite-install --make-rescue-image FILE        # the rescue USB disk as an image file
tools/installer/starbite-install --make-rescue-usb /dev/diskN [--image FILE] [--real]
tools/installer/rehearse.sh                                      # everything below against fakes (no phone)
tools/installer/starbite-install --bundle B --serial SERIAL ...   # name the phone (required with --real): section 15
tools/installer/starbite-install --bundle B --serial S --real --backup-only   # step 1 alone, every file re-checked
tools/installer/starbite-install --check-backup DIR              # a backup re-read on this computer (no phone)
tools/installer/mk-first-bundle.py OUT --boot-from ... --loader ... --env-dir ... --store-from ... --catalog ...
tools/installer/rehearse-bundle.sh BUNDLE                        # one real bundle through the first-install flow (fakes)
python3 -m unittest discover -s tools/installer/tests            # unit tests
```

Python 3 standard library only (3.11+ for `tomllib`; older Pythons use the built-in fallback parser).
macOS and Linux. Host tools it calls: `adb` (Android platform-tools), `fastboot` for a phone in the
loader's fastboot, `heimdall` only for a phone in Download mode, `ssh` + `ssh-keygen` (the installer
environment), `bash`, `gzip`, `shasum`/`sha256sum`. `adb` only to send a stock Android to Download mode.

Options: `--replace KEY` (repeatable) / `--replace-all` overwrite targets that hold data; `--erase-card`
erases a microSD with another layout; `--fresh-backup` takes a new backup even if a complete one exists;
`--rescue-usb DEV` writes the rescue stick at the end; `--state-dir DIR` (default `build/installer/`);
`--no-reboot`; `--yes` (dry run and rehearsal only; refused with `--real`).

## 2. Design

```
Mac / Linux PC                                       Galaxy S9
starbite-install ──ssh (USB ECM 172.16.42.1)───────► the installer environment (Linux, RAM only)
   │   stock Android: adb reboot download; Download mode: PIT check + heimdall flash --BOOT (loader);
   │   the loader's fastboot: fastboot boot installer.image installer.initrd --dtb installer.dtb
   ├─ welcome + 2 confirmations
   ├─ phone state: environment | Starbite fastboot | Download mode | stock Android (| TWRP: legacy only)
   ├─ preflight (read only): model, bootloader, battery, UFS size, protected areas, UFS layout, card size
   ├─ plan screen: every step, what it writes where, by which tool, and why (write/skip/keep/replace)
   ├─ dry run (default)  ── prints every command, writes nothing, records "dry run done" in the journal
   └─ real (--real) / rehearsal (--rehearse, fakes only): typed "install phone-XXXXXXXXXX", then the steps
```

- **Transport (v2): the Starbite installer environment** (section 13), a Linux kernel + initramfs that
  the loader's fastboot starts from RAM, reached by ssh over USB networking; `tools/installer/transport/adb`
  maps adb's CLI onto it, so every existing writer runs unchanged. Download mode writes BOOT only (the
  loader). **RECOVERY is never written or booted.** `--transport twrp` keeps v1's TWRP adb for S9 #1's
  own maintenance. A dry run never flashes or reboots; starting the environment from RAM writes nothing
  and is allowed in a dry run.
- **Existing tools do the writing** wherever one exists (section 4); the installer adds the checks before,
  the journal, and an independent read-back after each one.
- **Modes.** `--dry-run` is the default. `--real` and `--rehearse` both need a finished dry run of the same
  manifest (by SHA-256) on the same phone (journal), and a typed confirmation. `--rehearse` refuses unless
  the `adb` on PATH is the fake one (`--fake-root DIR` puts `tools/installer/fakes/` first on PATH).
- **Safety rules, enforced in code:**
  - the only way anything is written is an item destination (`sbinst/manifest.py parse_dest`), and the
    grammar cannot name EFS, CPEFS, PARAM, UP_PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA*, DTB, DTBO, the
    raw disks (GPT) or the boot LUNs; ODM only as a fresh `mkfs` (LineageOS's /metadata), or as One UI's
    odm image with the `starbite-oneui` UFS layout (section 14); card sector 0 only through the card
    layout step;
  - backup first: the protected areas, BOOT, RECOVERY, MISC, RADIO, OMR, ODM, the first 4 MiB of sda
    (protective MBR, GPT, PIT area), the backup GPT and the boot LUNs sdb-sde go to
    `build/installer/<phone-id>/backups/<time>/`, each hashed on the phone and on the computer;
  - data is never replaced silently: an item marked `data = true` (OS roots, Android /data, Windows, card
    images) is written only if its target is blank, or was written by this installer and interrupted, or
    with `--replace KEY` / `--replace-all`. LineageOS's /data and /metadata are renewed together or not at
    all (One UI has no /metadata: its /data stands alone);
  - every write is read back (SHA-256 computed on the phone); a mismatch stops the run (`STOPPED`, exit 3);
  - BOOT is written last (on a stock phone it was written first, in Download mode, and is then skipped as
    identical); RECOVERY is never written (refused as a destination, by the heimdall wrapper and by the
    transport);
  - the GPT is written only by the repartition step, with `--repartition`, behind its own verified backup
    (section 9);
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
`installer-manifest.example.toml`, `installer-manifest.oneui.example.toml` (section 14), `fakes/` (fake phone,
adb, fastboot, heimdall, ssh, rehearsal bundles), `tests/`, `rehearse.sh`.

## 3. The manifest (`installer-manifest.toml`, format 1)

A bundle is a directory: `installer-manifest.toml` plus the files it names. Full example (the main phone's
set): `tools/installer/installer-manifest.example.toml`. One UI 8 on the `starbite-oneui` layout:
`tools/installer/installer-manifest.oneui.example.toml` (section 14).

| Key | Meaning |
|---|---|
| `format = 1` | required |
| `name` | shown on the screens |
| `catalog` | optional: a copy of `tools/store/catalog.toml` in the bundle. Then the plan lists every visible menu entry as `covered` or `MISSING` (its store files must be in the bundle's stores, read with `sbstore2.py`; its root must be an item) and warns when the UFS store's catalog differs from it |
| `[phone]` | `models` (SM-G960F), `devices` (starlte), `ufs_blocks` (15616000), `min_battery` (50) |
| `[card]` | `min_bytes` (at least 60,000,000,000: a card sold as 64 GB holds 61.9-64.1 GB), `layout` (`"sd128"` S9 #1's 128 GB card, `"sd64"` any card of at least 64 GB: section 11; p1, p2 and the root sectors then come from `tools/store/cardlayout.py`, and `--verify-bundle` checks the catalog roots and the payloads' `sdroot=` against it), `sectors` (the exact card this layout was made for; card roots sit at fixed sectors in payload command lines; not with `sd64`), `p1`/`p2` `{type, start, sectors[, label]}`: the MBR the card layout step writes (with `layout`: may be left out, must agree if given) |
| `[ufs]` | `layout`: a target of `sbinst/ufslayout.py` (`starbite-a`, `starbite-a16g`, `starbite-oneui`). The phone is repartitioned to it with `--repartition` (section 9) |
| `[env]` | `cmdline` for the installer environment (default `sb.installer=1`) |
| `[[layout]]` | `name`, `node`, `start`, `blocks` (4 KiB blocks): v1's check. With `[ufs] layout` optional and must agree with it (the filling row by start only) |
| `[[item]]` | one thing to install, below |

| Item key | Meaning |
|---|---|
| `key` | unique, `[a-z0-9-]`; `--replace KEY` names it |
| `title`, `os`, `note` | shown; `os` (starbite, android, linux, chromeos, windows, macos, ios) drives the no-password list |
| `file`, `size`, `sha256` | path inside the bundle (no `..`, no symlink out), byte size, SHA-256. `--update-hashes` fills the last two |
| `dest` | `part:NAME` (BOOT SYSTEM VENDOR A16DATA WINESP WINDOWS; ODM only with `action = "mkfs-metadata"`, or with `action = "write"` when `[ufs] layout = "starbite-oneui"`; never RECOVERY), `env:kernel|initrd|dtb` (the installer environment, RAM only), `slot:USERDATA@OFFSET` (the 16 GiB Apple slot), `store:ufs`, `store:sd`, `card:lba:SECTOR` (inside `[card] p2`, 1 MiB aligned, no overlaps; with a card layout inside one of its root slots), `card:root:KEY` (with `[card] layout`: the root slot of KEY, e.g. `card:root:mac13`, resolved to `card:lba:` at load), `card:file:/PATH` (a file on card p1's exFAT) |
| `action` | `write` (default); `mkfs-metadata` (ODM only: LineageOS's /metadata, a fresh ext4 by `metadata-reset.sh`); `mkfs-oneui-data` (A16DATA only: One UI's /data, a fresh ext4 made on the phone, section 14). The two `mkfs` actions take no `file` and are always user data (`data` is forced on) |
| `data` | `true`: the target holds user data once its OS runs (never replaced without `--replace`) |
| `optional` | `true`: a missing file skips the item instead of refusing the bundle |

Checks at load: unknown destination or a protected name refuses; sizes must be 4096-aligned for UFS and
MiB-aligned for card ranges; slot items stay below 16 GiB; card ranges inside p2 and not overlapping;
duplicate keys or destinations refuse; an unknown action, a `mkfs` action on another partition or with a
`file`, an ODM image without `[ufs] layout = "starbite-oneui"`, and `mkfs-oneui-data` together with
`mkfs-metadata` in one bundle refuse. `--verify-bundle` then checks presence, size and SHA-256 of every
file (hashes cached in `build/installer/hash-cache.json` by path, size and mtime).

## 4. The steps (fixed order)

| # | Step | Writes | Tool | Verified by |
|---|---|---|---|---|
| 1 | Back up | the computer only | `tools/android/pixel/part-backup.sh mac` per partition; raw head/tail/LUNs by the installer | phone SHA-256 == local SHA-256 for each |
| 2 | UFS partition table | sda LBA 1-5 + the last 5 LBAs, only with `--repartition` | `sbinst/gpt.py` + `repart.py` (section 9) | both copies read back and parsed; the kernel's by-name/start/size == the plan |
| 3 | microSD MBR | card sector 0, p2's first 3 blocks (old store header) | installer (`dd` of a 512-byte MBR built on the host), `blockdev --rereadpt` | MBR read back byte for byte; `/sys/class/block/mmcblk0p1/start` |
| 4 | microSD p1 exFAT | p1 | the environment's `mkfs.exfat -n STARBITE` | "EXFAT   " read back from p1's boot sector |
| 5 | Apple slot (iOS 16.5) | USERDATA 0..16 GiB | `tools/refport/slot-write.py` | range SHA-256 on the phone |
| 6 | UFS OS store | USERDATA +16 GiB | `tools/store/store-flash.sh` (keeps the phone's settings block) | store area == image except block 2 |
| 7-8 | SYSTEM, VENDOR | partitions | `tools/android/pixel/part-write.py --state` | the tool, then range SHA-256 again |
| 9-10 | LineageOS: A16DATA, ODM | partitions | `part-write.py`; `metadata-reset.sh` (the environment's mke2fs) | range SHA-256; `blkid` TYPE ext4 LABEL metadata |
| 9-10 | One UI: ODM, then A16DATA | partitions | `part-write.py` (the odm image); the phone's `mke2fs -t ext4 -F -L data -b 4096 NODE BLOCKS-5` (section 14) | range SHA-256; `blkid` TYPE ext4 LABEL data + `dumpe2fs -h` Block count == partition blocks - 5 |
| 11-12 | WINESP, WINDOWS | partitions | installer chunk writer | range SHA-256 |
| 13 | card OS store | card p2 | `tools/store/store-flash-sd.sh` | store area == image except block 2 |
| 14-15 | macOS Ventura, iOS 26 roots | card sector ranges | `tools/macos/card-range-flash.py --state` | range SHA-256 |
| 16-18 | pmOS, Ubuntu, ChromiumOS images | files on p1 (`/external_sd/distros/`) | installer chunk writer into `NAME.part`, then `mv` | `sha256sum` of the file |
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

The example manifest lists the main phone's set: BOOT, the installer environment (`env:kernel`,
`env:initrd`, `env:dtb`: RAM only), both stores (the UFS store with the catalog,
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
| One UI 8 | A16DATA is a fresh ext4 made on the phone (`mkfs-oneui-data`); no /metadata. Setup is finished by `tools/android/oneui/first-boot.sh` (docs/ANDROID-ONEUI.md 2) | yes | the same journal check as Android 17 |
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

- Unit tests (`python3 -m unittest discover -s tools/installer/tests`, 137 tests): v1's (disk safety with
  recorded diskutil plists and fake sysfs trees, the destination allowlist, manifest and bundle checks,
  the TOML parser, the ext4 reader and no-password checks, journal / chunk state / resume, plan
  classification, MBR round trip, the card layouts of section 11) plus `test_gpt.py` (the GPT engine on
  S9 #1's real backup and on independently written stock GPTs, the layout table's rules, every
  interrupted-write state, the PIT check: section 9.4), `test_bundler.py` (the deny list, streaming with
  holes and the zeroed settings block, `--from-local` with a hard link and a .gz source, `[ufs]` in a
  manifest), `test_no_recovery.py` (RECOVERY refused as a destination, by the heimdall wrapper, by the
  transport shim; no code path flashes or boots it) and `test_oneui.py` (One UI 8: section 14.4).
- `tools/installer/rehearse.sh`: the real flow end to end against fake phones and fake disks
  (`fakes/fakephone.py`: a sparse 64 GB UFS file holding a real GPT, built from `ufslayout.py`, with the
  partition nodes made from it the way Linux 4.9's efi.c does (an invalid primary = no partitions);
  Samsung's small partitions filled with random bytes; a sparse card; a PIT for heimdall; stock or
  Starbite firmware) through the fake `adb`, `fastboot` (`boot` of the installer environment, `oem
  boot-next`), `heimdall` (sessions, `download-pit`, BOOT only) and `ssh` (the environment; only the key
  appended to the initrd). The existing writers run unchanged through `transport/adb`. Fake limits:
  partition nodes are separate files, `[ -b` is read as `[ -e`, mke2fs/dumpe2fs/tune2fs/blkid/mount/
  mkfs.exfat/getprop/blockdev are stand-ins, and the environment is the host's bash, not the real initramfs.
  Sections: 1-9 the main phone's bundle (refusals, dry run, a cable pull in SYSTEM and the resume, nothing
  left to do, the phone without a journal); 10 Download mode (BOOT only, RECOVERY untouched); 11 the
  loader's fastboot and the legacy TWRP transport; 12 preflight refusals; 13 the rescue stick; 14 the 64 GB
  card; **15 a stock phone, the whole way** (excerpt below); 16 One UI 8 on the `starbite-oneui` layout
  (section 14.4).

Result 2026-11-07 (macOS, with section 16): **175 rehearsal checks passed, 0 failed; 137 unit tests OK**.
Result 2026-11-06 (macOS): **144 rehearsal checks passed, 0 failed; 111 unit tests OK**. Section 15 as it printed (plan rows and
warnings left out; paths shortened):

```
######## 15. a STOCK phone: Samsung firmware and GPT, unlocked bootloader, a 64 GB card (docs/INSTALLER.md 9, 13)
fake phone in <repo>/build/installer-rehearsal/phone-stock: state android, stock firmware, SM-G960F, GPT stock, card blank (124735488 sectors)
before: kernel: USERDATA primary 6cd38d4010bf backup 7c3be94dbfe8
ok  card layout sd64: catalog roots and payload sdroot= match (mac13 139264)
ok  bundle verified: 16 items; optional items not in it: windows
  found: android (adb)
  $ adb reboot download  [dry run: not run]
== The plan (no phone: targets not checked) ==
ok  offline dry run: nothing was read from or written to a phone
PASS dry run from stock Android: the way in is Download mode (exit 0)
PASS stock firmware recognised (ro.build.PDA)
PASS shows adb reboot download, does not run it
ok  card layout sd64: catalog roots and payload sdroot= match (mac13 139264)
ok  bundle verified: 16 items; optional items not in it: windows
  found: download (Samsung Download mode (04e8:685d))
REFUSED: the PIT is not an SM-G960F's (an S9+ SM-G965F, or another phone): nothing was flashed
PASS an S9+ (SM-G965F) in Download mode is refused by its PIT (exit 2)
PASS names the partition that differs
PASS ...and says why
PASS nothing was flashed to the S9+ (BOOT, RECOVERY)
ok  card layout sd64: catalog roots and payload sdroot= match (mac13 139264)
ok  bundle verified: 16 items; optional items not in it: windows
  found: android (adb)
  $ adb reboot download
ok  PIT: an SM-G960F (build/installer-rehearsal/state-stock/download/pit-20261106-150014.bin)
  $ heimdall flash --BOOT <repo>/build/installer-rehearsal/bundle64/boot/starbite-boot.img --no-reboot
ok  the Starbite loader is in BOOT. Now the keys (above).
ok  the Starbite loader's fastboot is up
ok  the installer environment is up: ssh root@172.16.42.1 (USB networking)
REFUSED: no finished dry run of this bundle on phone-5c7ed06646: run the same command with --dry-run first (it writes nothing), then again
PASS rehearsal from stock Android: adb reboot download, PIT, BOOT via heimdall, loader fastboot, the environment, then the dry-run gate (exit 2)
PASS the PIT is an SM-G960F's
PASS tells the user exactly which keys to hold
PASS ...and how to reach the loader's fastboot
PASS the loader's fastboot came up
PASS the installer environment runs from RAM
PASS then asks for the dry run
ok  card layout sd64: catalog roots and payload sdroot= match (mac13 139264)
ok  bundle verified: 16 items; optional items not in it: windows
  found: env (Starbite installer environment (ssh root@172.16.42.1))
ok  the Starbite installer environment is up (USB networking, ssh)
== The plan ==
REFUSED: the plan has blocked steps: UFS partition table (starbite-a): the phone has another partition table; USERDATA, WINESP, WINDOWS, A16DATA would get new places or sizes and lose their contents (a stock phone: its Android data): run again with --repartition
PASS dry run in the environment without --repartition: the GPT step is BLOCKED (exit 2)
PASS UFS partition table BLOCKED
PASS says how to allow it
ok  card layout sd64: catalog roots and payload sdroot= match (mac13 139264)
ok  bundle verified: 16 items; optional items not in it: windows
  found: env (Starbite installer environment (ssh root@172.16.42.1))
ok  the Starbite installer environment is up (USB networking, ssh)
== The plan ==
  $ adb shell "blockdev --rereadpt /dev/block/sda; sb-byname"; then by-name, start and size of every partition == the plan  [dry run: not run]
  $ python3 -I tools/android/pixel/part-write.py SYSTEM build/installer-rehearsal/bundle64/android/system.img --state build/installer-rehearsal/state-stock/phone-5c7ed06646/chunks/android-system-420486d19782.state  [dry run: not run]
  $ python3 -I tools/android/pixel/part-write.py VENDOR build/installer-rehearsal/bundle64/android/vendor.img --state build/installer-rehearsal/state-stock/phone-5c7ed06646/chunks/android-vendor-23991468e55b.state  [dry run: not run]
  $ python3 -I tools/android/pixel/part-write.py A16DATA build/installer-rehearsal/bundle64/android/a16data.img --state build/installer-rehearsal/state-stock/phone-5c7ed06646/chunks/android-data-86f4b074e488.state  [dry run: not run]
  written 0, skipped 2, kept 0; 0 write commands ran
== Dry run finished ==
ok  nothing was written (26 write commands printed). The real mode (--real, or --rehearse on a fake phone) is now allowed for this bundle on this phone.
PASS dry run with --repartition: old and new tables side by side, nothing written (exit 0)
PASS dry run: the fake phone is byte-identical
PASS USERDATA: stock size -> 18.75 GiB, side by side
PASS A16DATA new
PASS the protected rows identical
PASS plan: repartition
PASS A16DATA written fresh (no --replace needed)
ok  card layout sd64: catalog roots and payload sdroot= match (mac13 139264)
ok  bundle verified: 16 items; optional items not in it: windows
  found: env (Starbite installer environment (ssh root@172.16.42.1))
ok  the Starbite installer environment is up (USB networking, ssh)
== The plan ==
ok  sda-head-4M: 4.0 MiB, sha256 e69ec3fc1a8815ea... (phone and local agree)
ok  sda-tail-gpt: 20.0 KiB, sha256 7c3be94dbfe8f023... (phone and local agree)
ok  sdb: 4.0 MiB, sha256 3d13313feedeb063... (phone and local agree)
ok  sdc: 4.0 MiB, sha256 9c73e18cf4f3f9bb... (phone and local agree)
ok  sdd: 2.0 MiB, sha256 b06546bb288fdc71... (phone and local agree)
ok  sde: 2.0 MiB, sha256 e25666ae08e632e7... (phone and local agree)
ok  backup complete: 24 items in build/installer-rehearsal/state-stock/phone-5c7ed06646/backups/20261106-150021
ok  GPT backup: both copies valid, phone and computer agree (head 1ef05c28f825..., tail 7c3be94dbfe8...) in build/installer-rehearsal/state-stock/phone-5c7ed06646/gpt
ok  backup GPT copy written and read back (20480 B at LBA 15615995, sha256 5f676eaaf798...)
STOPPED: adb shell : sbinst-gpt-primary; (trap "" HUP INT; dd if=/tmp/sbinst-gpt/primary.bin of=/dev/block/sda bs=4096 seek=1 conv=notrunc,fsync 2>/tmp/sbinst-gpt/primary.err; echo $? > /tmp/sbinst-gpt/primary.rc. failed:  fake adb: simulated cable pull
PASS rehearsal: a cable pull between the backup and the primary GPT copy (exit 3)
PASS the whole GPT backed up and verified first
PASS the backup copy written first
PASS stopped at the primary copy
  after the pull: kernel: USERDATA primary 6cd38d4010bf backup 5f676eaaf798
PASS the kernel still sees the stock table (the old primary is valid)
PASS the original GPT is saved on this computer (anchor.json new-backup.bin new-primary.bin orig-head.bin orig-tail.bin )
ok  card layout sd64: catalog roots and payload sdroot= match (mac13 139264)
ok  bundle verified: 16 items; optional items not in it: windows
  found: env (Starbite installer environment (ssh root@172.16.42.1))
ok  the Starbite installer environment is up (USB networking, ssh)
== The plan ==
ok  primary GPT copy written and read back (20480 B at LBA 1, sha256 a4030756a82d...)
ok  both GPT copies on the phone are the plan (CRCs, cross references, entries)
ok  the kernel sees the starbite-a table: 28 partitions, by-name nodes, starts and sizes as planned
ok  card MBR written and read back; partitions re-read
ok  p1 formatted exFAT "STARBITE"
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
  written 12, skipped 3, kept 0; 34 write commands ran
PASS the same command again: writes the primary, reboots, installs everything (exit 0)
PASS the GPT step resumes
PASS writes the primary copy
PASS ...and not the backup copy again
PASS after the re-read the kernel sees the new table
PASS A16DATA written
PASS BOOT: skipped, identical (it went in first, in Download mode)
  now: kernel: USERDATA WINESP WINDOWS A16DATA primary a4030756a82d backup 5f676eaaf798
PASS four partitions after SYSTEM..CP_DEBUG: USERDATA WINESP WINDOWS A16DATA
PASS protected areas, Samsung's other partitions, sda's PIT area, sdb-sde: byte-identical
  == backup/20261102-185306 (S9 #1), byte for byte
PASS the new GPT is S9 #1's table exactly (both copies, every byte)
PASS RECOVERY untouched through the whole install (Samsung's stock recovery)
ok  card layout sd64: catalog roots and payload sdroot= match (mac13 139264)
ok  bundle verified: 16 items; optional items not in it: windows
  found: env (Starbite installer environment (ssh root@172.16.42.1))
ok  the Starbite installer environment is up (USB networking, ssh)
== The plan ==
  written 0, skipped 10, kept 8; 0 write commands ran
PASS and again: nothing left to do (exit 0)
PASS the GPT step: skip
PASS 0 write commands
ok  dry run: nothing written
PASS --restore-gpt dry run: shows how the stock table would go back (exit 0)
PASS the exact restore command
ok  phone: SM-G960F, card layout sd64; 6 items to read (74.3 MiB)
ok  boot: 55.0 MiB, sha256 d6fda8f9819a... (phone and computer agree)
ok  ufs-store: 1.2 MiB, sha256 e76799766a39... (phone and computer agree)
ok  sd-store: 92.0 KiB, sha256 850806203210... (phone and computer agree)
ok  pmos: 8.0 MiB, sha256 ab2f5adbdc95... (phone and computer agree)
ok  ubuntu: 8.0 MiB, sha256 9f94c6a8dcfe... (phone and computer agree)
ok  chromeos: 2.0 MiB, sha256 ecaf5db99c7f... (phone and computer agree)
ok  <repo>/build/installer-rehearsal/bundle-built/installer-manifest.toml: 7 items, 74.3 MiB; verified
PASS the bundle builder reads the installed phone (read only) into a new sd64 bundle (exit 0)
PASS every item hashed on the phone and here
PASS manifest written
ok  card layout sd64: catalog roots and payload sdroot= match (mac13 139264)
ok  bundle verified: 7 items
PASS ...and it verifies as a bundle (exit 0)
PASS its stores carry the sd64 card layout

######## result: 144 passed, 0 failed
```

## 9. Repartitioning: stock Samsung GPT to the Starbite table (v2)

Written 2026-11-06. A stock SM-G960F has Samsung's GPT: after Samsung's 24 partitions, one USERDATA runs
to the end of the disk. Starbite splits that space (USERDATA 18.75 GiB with the iOS 16.5 slot and the OS
store, WINESP, WINDOWS, A16DATA). v2 does this as step 2 of the install, between the backup and the first
OS write, with `--repartition` (without it the step is BLOCKED and says why).

### 9.1 One table: `tools/installer/sbinst/ufslayout.py`

Every UFS table the installer knows is one Python tuple of rows; nothing else in the installer, the fakes
or the tests spells a partition number out. `python3 tools/installer/sbinst/ufslayout.py show [LAYOUT]`
prints one, `... check` validates all.

| Layout | What | Target? |
|---|---|---|
| `stock` | Samsung's table: rows 1-24 + USERDATA to the end | reference only (the fake stock phone; never written) |
| `starbite-a` | S9 #1 today (layout A, docs/ANDROID-PIXEL.md 2.1). `ACTIVE` | yes |
| `starbite-a16g` | Phase 2 of docs/ANDROID-PIXEL.md 2.3: WINDOWS 17.84 GiB, A16DATA 16 GiB (Linux data type) | yes |
| `starbite-oneui` | One UI 8 (docs/ANDROID-ONEUI.md 4; S9 #1 since 2026-11-07): SYSTEM 6.14 GiB, WINDOWS 22 GiB, VENDOR (sda19) and ODM (sda20) behind WINDOWS (entries out of block order), A16DATA 10.78 GiB (Linux data type). Its bundles: section 14 | yes |

A row is `Part(name, start, blocks, content, role, type, guid, attrs)`, in 4 KiB blocks (sda's logical
block: its GPT header sits at byte 4096; `/sys/block/sda/queue/logical_block_size` = 4096, checked in
preflight). `start=None` = after the previous row on 1 MiB; `blocks=FILL` = to the last usable block
(only the last row: it absorbs UFS size differences between phones). The row order is the GPT entry
order (row N = `/dev/block/sdaN`). `content` says what moving a row means:

| content | rows (starbite-a) | moved or resized |
|---|---|---|
| `protected` | 1-17: BOTA0 BOTA1 EFS PARAM UP_PARAM KEYSTORAGE BOTA2 DTB DTBO BOOT RECOVERY RADIO NAD_REFER PERSISTENT MISC STEADY DQMDBG (`SAMSUNG_HEAD`) | never: index, name, start, size, type GUID, unique GUID and attributes must equal the phone's exactly, or the installer refuses |
| `keep` | CACHE HIDDEN OMR CP_DEBUG | refused (nothing rewrites them) |
| `installer` | SYSTEM VENDOR ODM USERDATA WINESP WINDOWS A16DATA | allowed: the installer writes each one fresh from the bundle |
| `scratch` | stock USERDATA | allowed: contents not needed |

Every row carries a one-line `role` (what it holds, who reads or writes it), shown by `show`. Unique
GUIDs: Samsung's rows have Samsung's fixed ones ("ANDROID " + the name); the Starbite rows carry S9 #1's,
so a phone laid out with `starbite-a` gets **the same GPT bytes as S9 #1** (checked against the real
backup, 9.4); a new row without a pinned GUID gets one derived from its name, the same on every phone. A
bundle made from the main phone (Windows' BCD, any `root=PARTUUID=`) therefore holds on every phone.

**A new layout** (as `starbite-oneui` was made): copy `_starbite_a()`, edit the rows from SYSTEM on (never
`SAMSUNG_HEAD`), register it in `LAYOUTS` and `TARGETS`, point `ACTIVE` at it (or name it in a bundle: `[ufs]
layout = "NAME"`), run `ufslayout.py check` and the unit tests. If SAMSUNG's CACHE/HIDDEN/OMR/CP_DEBUG have to
move, change their `content` to `scratch` on purpose; the engine refuses otherwise.

### 9.2 The engine: `tools/installer/sbinst/gpt.py`

Pure Python, bytes in, bytes out (`python3 tools/installer/sbinst/gpt.py show|plan HEAD TAIL [LAYOUT]`
works on any backup):

- `read()` checks both copies: protective MBR, signature, header CRC over the header's own size (Samsung
  writes 512, not 92), entry-array CRC, the LBA cross references (primary 1 / alternate last, backup the
  other way), the same disk GUID / usable range / entry geometry in both, both entry arrays byte-identical,
  no overlaps.
- `plan()` builds the new table from a layout: refuses unless the 17 protected rows match exactly, refuses
  a partition on the phone the layout does not know, refuses moving a `keep` row; keeps every header field
  (revision, header size, disk GUID, usable range, entry geometry) and only renews the CRCs; never writes
  the protective MBR or anything after LBA 5 (Samsung's PIT area). Then a byte-by-byte check: only the CRC
  fields and the entries from SYSTEM on may differ, and the result must read back as exactly the planned
  entries. `plan.fresh` = rows whose place or size changed (their contents are gone).
- `classify()` / `next_action()`: each copy on the phone is `orig` (the saved backup), `new` (the plan),
  `invalid` or `other`; the pair says what to write (table below), anything else is a refusal.

### 9.3 The step on the phone (`tools/installer/sbinst/repart.py`)

1. Preflight (read only): both copies read (`dd` of LBA 0-5 and the last 5 LBAs), parsed, planned. The
   plan screen shows the old and the new table side by side (rows 1-17 folded as "identical").
2. **Backup first**: both copies read again with the phone's own SHA-256 next to this computer's, parsed
   (both valid), compared with the preflight read, saved as `<state>/<phone-id>/gpt/orig-head.bin`,
   `orig-tail.bin` (+ the planned `new-*.bin` and `anchor.json`), and next to the step-1 backups. This
   saved copy is the "orig" every later run compares with.
3. Typed confirmation `repartition phone-XXXXXXXXXX`; the journal records the fresh rows, and earlier
   records of items on them are dropped.
4. Both copies pushed to `/tmp/sbinst-gpt/` and checked by SHA-256 on the phone.
5. **The backup copy first** (`dd ... seek=<last-5> conv=notrunc,fsync`), read back; **then the primary**
   (LBA 1-5), read back; then both copies read and parsed together.
6. The kernel re-reads the table (installer environment: `blockdev --rereadpt /dev/block/sda; sb-byname`,
   since nothing there mounts sda; if that does not take, the environment restarts through the loader's
   fastboot; legacy TWRP: `adb reboot recovery`), and every partition's by-name node, start and size is
   checked against the plan.
7. Every item on a fresh row is written fresh (no `--replace` needed: `--repartition` was the consent); an
   item leaves the fresh list once written, so data an OS writes later is never mistaken for a fresh place.

**Why backup first, then primary (the cable-pull answer).** Linux (`block/partitions/efi.c`) uses the
primary whenever it is valid and never uses a valid backup's contents without `gpt=force`; with an
invalid primary it shows no partitions at all. So the primary write is the commit point: until it lands,
every reader (the kernel, S-Boot's lookups, the next boot) still sees the old table. Each copy is one
`dd` started in a background subshell that ignores SIGHUP, so a cable pull cannot stop it half way; only
a power loss inside that millisecond window can tear a copy (preflight wants 50 % battery).

| primary | backup | what happened | next run |
|---|---|---|---|
| orig | orig | nothing written yet | write both |
| orig | invalid | the backup write was torn (power loss) | write both |
| orig | new | **cable pull between the copies** | write the primary only |
| invalid | new | the primary write was torn (power loss) | write the primary from the plan |
| new | new | done (if the kernel has not re-read: re-read) | nothing |
| other | any | someone else changed the table | refused; `--restore-gpt` or Odin with the stock PIT |

`--restore-gpt [--real]` writes the saved original back the same way (backup copy, then primary, read
back, re-read), after a typed `restore gpt phone-XXXXXXXXXX`.

### 9.4 Checked against the real backup

`tests/test_gpt.py` (20 tests) reads S9 #1's `backup/20261102-185306/sda-head-4M.img` + `sda-tail-gpt.img`
(read only; found in this checkout or the main one, or `$SBINST_GPT_BACKUP`; the tests skip without it):

- it parses: 4096-byte blocks, header size 512, 128 x 128 entries, usable 6..15615994, both copies valid;
- `starbite-a` planned on it changes nothing (every byte equal);
- a stock table (S9 #1's rows 1-24, one USERDATA to the end, random GUIDs from SYSTEM on, written by an
  independent writer in the test) planned to `starbite-a` gives **exactly S9 #1's GPT bytes** (both copies);
- `starbite-a` -> `starbite-a16g` equals `tools/android/pixel/gpt-a16grow.py`'s output (which was
  cross-checked with `sgdisk -v` by gpt-crosscheck.sh) byte for byte;
- CRC round trip; a flipped byte in either header or either entry array is caught; a wrong disk size too;
- refusals: EFS moved, BOOT resized, PARAM's type or KEYSTORAGE's GUID changed, an unknown partition, a
  layout that moves OMR, a 512-byte-sector disk;
- a bigger UFS: the last row fills it and the backup copy moves with the disk's end;
- every interrupted state of the table above, built from real torn copies.

The rehearsal (section 8, part 15) writes it on a fake stock phone through the whole flow, with the cable
pull between the copies, and checks that the result is S9 #1's GPT byte for byte.

## 10. Implemented vs stubbed, and what needs the real phone (v2)

**Implemented and rehearsed (fakes):** everything in v1's list (the CLI and screens, confirmations,
preflight, plan, dry run and its gate, backup, card MBR + exFAT, every writer, read-back, journal and
resume, A16DATA+ODM pairing, no-password checks, rescue stick) plus: stock Android -> `adb reboot download`;
Download mode with `heimdall detect`, `download-pit` and the PIT check (an S9+ PIT is refused before any
write), heimdall's `--resume` session rule; the Starbite loader into BOOT (the only Download-mode write);
the loader's fastboot -> the installer environment from RAM (key appended to the initrd, ssh, the adb
transport shim); the GPT engine, the layout table, the repartition step with its backup, the cable-pull
resume and `--restore-gpt`; the bundle builder (`--from-phone`, `--from-local`, sd64 retargeting of the
stores); RECOVERY never written or booted (refused in the manifest grammar, in the heimdall wrapper, in the
transport shim, in the fakes; tests/test_no_recovery.py).

**Stubbed or not done:**
- **The installer environment is built but has not run on the phone** (2026-11-06: it is Starbite
  Maintenance in installer mode, docs/MAINTENANCE.md; `mk-env.sh` writes `installer.image/.initrd/.dtb`).
  Its by-name tree now gives real `/dev/block/sdaN` nodes (the first version linked into `/dev`, which
  `part-write.py`'s `readlink -f` check would have refused). No bundle carries it yet.
- No real bundle exists; `--build-bundle --from-phone` is the way to make one (section 12) once the
  environment is built (the main phone must run the environment, or TWRP with `--transport twrp`).
- On a stock phone the stock BOOT image is replaced in Download mode **before** the installer can back it
  up (heimdall cannot read partitions); it is Samsung's `boot.img` from the firmware (G960FXXUHFVG6 AP).
  Everything else is backed up from the environment before anything else is written.
- Card layouts (section 11): unchanged; macOS Ventura's password (section 6); no clean Windows image;
  UP_PARAM / VG6 steps; the rescue stick's content.

**Needs the real phone to verify (in this order):**
1. `heimdall download-pit` on a stock SM-G960F: the PIT's units (4 KiB blocks or 512-byte sectors: both
   accepted, the one found is printed) and that its names match `SAMSUNG_HEAD`; heimdall's `--resume` rule.
2. `heimdall flash --BOOT starbite-boot.img --no-reboot` on a stock phone, then the keys: Volume Up alone at
   the loader's start (S-Boot ignoring it is unverified, docs/USB-FASTBOOT.md 9.5) or Bixby > USB FASTBOOT.
   The loader with **no OS store** on UFS reaching its fastboot (cat_load fails first; the code ignores it).
3. The first **Linux** `fastboot boot` (only an XNU payload has run, 2026-11-05): kernel + initrd + DTB in
   one v2 boot image, the DTB's bootargs and `/chosen linux,initrd-*` placeholders, total under 0x04ff0000.
4. The environment itself: dwc3 in device mode from the loader's handoff, the ECM link on the Mac
   (172.16.42.2 by DHCP), dropbear with the appended key, `/dev/block/by-name` from PARTNAME, the facts
   (`getprop` stand-in, `/sys/class/power_supply/*/capacity` for the battery), exfat/f2fs/ext4 tools.
5. `blockdev --rereadpt /dev/block/sda` taking the new table in place (the fallback restarts the environment).
6. The GPT write itself on UFS (both copies, read back) and S-Boot booting with the new table; S9 #1 is the
   evidence that S-Boot keeps a GPT that differs from its PIT (USERDATA was split on 2026-10-25).
7. Speed of every multi-GiB write and of `--build-bundle --from-phone` over USB networking.
8. v1's list items 5-13 (card formatter, mounts, the final reboot into the menu, no-password checks, the
   rescue stick), now in the environment instead of TWRP.
9. One UI 8 on the `starbite-oneui` layout: section 14.5.

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

## 12. The bundle builder

```sh
tools/installer/starbite-install --build-bundle DIR --from-phone [--card-layout sd64] [--only KEY] [--skip KEY]
tools/installer/starbite-install --build-bundle DIR --from-local [--sources FILE] [--card-layout sd64]
```

`--from-phone` (`sbinst/bundler.py`): the main phone runs the installer environment (`--bundle B
--start-env` starts it) or, legacy, TWRP. Every item is **read** (the builder's runner is a dry-run runner:
a write command could only be printed) and streamed straight into DIR: `adb exec-out "... | tee FIFO"`
with `sha256sum` reading the FIFO on the phone, so each item is read once and hashed on both sides; the
two hashes must agree or the item is dropped. Runs of zero MiB become holes. The stores lose their
settings block (block 2: default entry, last boot; zeroed). With `--card-layout sd64` the stores are rebuilt
for the 64 GB layout (`sbstore2.py build --import`, every SWEETXNU payload's `sdroot=` moved to its sd64
slot, payloads without a slot left out, the catalog from `cardlayout.catalog_text`) and the macOS root goes
to `card:root:mac13`, so one bundle from the 128 GB main phone installs on any card of 64 GB or more.
Then `installer-manifest.toml` (`[ufs] layout`, `[env]`, `[card] layout`, every item with size and
SHA-256, no `ufs_blocks`: the last row fills any UFS) and `--verify-bundle` on the result.

`--from-local`: the same items from local build outputs (each source's candidate paths in `SOURCES`,
under the main checkout when run from a worktree, `$SBINST_LOCAL_ROOT`, or a TOML `key = "path"` file via
`--sources`). A plain file on the same volume is hard-linked (no copy); a `.gz` is decompressed on the way.

**Never in a bundle** (code: `bundler.NEVER`, `check_source()`, tests/test_bundler.py): EFS, CPEFS, PARAM,
UP_PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA0-2, DTB, DTBO, RECOVERY, MISC, RADIO, OMR, CACHE, HIDDEN,
CP_DEBUG, NAD_REFER, DQMDBG, the raw LUNs, A16DATA (Android's /data: the fresh empty one comes from
make-a16data.sh, local only), ODM (made fresh on the phone), the stores' settings block. Carried with a
note: the iOS 16.5 slot (its /var is the main phone's) and Windows (its user profile).

**Where it can go.** DIR is any path: an external disk, the USB stick (exFAT or APFS; a FAT32 volume is
refused before the first 4+ GiB item), or iCloud Drive (`~/Library/Mobile Documents/com~apple~CloudDocs/`).
Every item is checked against the free space first. iCloud Drive keeps a local copy of each file until it
is uploaded: when the next item does not fit, the builder asks iCloud to evict the finished items
(`brctl evict`, which works once a file is uploaded) and waits up to 60 minutes, then stops (STOPPED: a
re-run keeps every finished item, `DIR/.build-state.json`). A full bundle read from the main phone is about
80 GB raw (whole partitions: SYSTEM 4.75 GiB, the 16 GiB slot, WINDOWS 25.8 GiB, the 15 GiB macOS root, ...),
so with this Mac's free space the USB stick or an external disk is the practical target; iCloud works item
by item only as fast as it uploads. Not tested on a real iCloud Drive or FAT32 stick (fakes only).

## 13. No TWRP: the loader and the installer environment

The user's rule (2026-11-06): **the installer never writes RECOVERY and never boots it**; the phone keeps
Samsung's stock recovery, and everything else is done from Starbite's own side.

**The flow on a stock phone:**
1. Stock Android with USB debugging: `adb reboot download` (or the user enters Download mode).
2. Download mode: `heimdall detect`, `heimdall download-pit` (read only) checked against `SAMSUNG_HEAD`
   (an S9+ or any other phone is refused here), then **`heimdall flash --BOOT starbite-boot.img
   --no-reboot`**: the only Download-mode write (typed `flash boot`). `Heimdall.run` refuses any other
   partition.
3. The user leaves Download mode (Volume Down + Power) and holds **Volume Up alone** while the phone starts
   (or Bixby after the Samsung splash > USB FASTBOOT): the Starbite loader's fastboot (18d1:4ee0).
4. The installer: `fastboot getvar serialno/product/version-bootloader` (must be `starbite-*`, `starlte`),
   `fastboot oem boot-next fastboot` (a reboot or crash of the environment comes back to fastboot), then
   `fastboot boot installer.image installer.initrd+key --dtb installer.dtb --header-version 2 --cmdline
   "sb.installer=1 sb.serial=SERIAL"`. A dry run does this too: it is RAM only.
5. The environment (`tools/installer/env/`: `init`, `sb-byname`, `getprop`, `sb-reboot`, built by
   `mk-env.sh` from Alpine + e2fsprogs exfatprogs f2fs-tools sgdisk dropbear util-linux coreutils and the
   static busybox): mounts nothing of the phone's storage, makes `/dev/block/NAME` and
   `/dev/block/by-name/PARTNAME` like Android, starts USB ECM (172.16.42.1, DHCP gives the computer
   172.16.42.2) + ACM, and dropbear on usb0 that takes only the key the installer appended to the initrd
   (an ed25519 key under `build/installer/env/`, one per computer, newc cpio `root/.ssh/authorized_keys`).
   No password, no telnet.
6. From here every step (backup, GPT, card, OS writes, store, BOOT last if it changed, the reboot into the
   menu with `sb-reboot menu`) runs against the environment. `tools/installer/transport/adb` goes first on
   PATH: adb's CLI subset on ssh (`shell`, `exec-out`, `push`, `pull`, `get-state`, `reboot` -> `sb-reboot`;
   `reboot recovery` refused), so the existing writers (part-write.py, part-backup.sh, slot-write.py,
   store-flash*.sh, card-range-flash.py, metadata-reset.sh) run unchanged.

`--transport twrp` keeps v1's TWRP adb for S9 #1's own maintenance; the default transport refuses a phone in
TWRP, and the legacy one does not start from Download mode.

**One environment (2026-11-06): the installer environment is Starbite Maintenance** (docs/MAINTENANCE.md) in
installer mode (`sb.installer=1`: the card stays unmounted; the appended key is merged with the built-in
ones). `tools/installer/env/{init,sb-byname,getprop,sb-reboot}` link to `tools/maint/env/`, `mk-env.sh` builds
SBMAINT and names its parts `installer.*`, and `transport/adb` also serves `SB_TRANSPORT=maint` for every
other host tool (it is reached as `tools/maint/transport/adb` too). The same environment can live in a
partition of its own (`SBMAINT`, proposed for the One UI layout) and in the OS store, so an installed phone
starts it from the loader without a computer-side push.

**What the loader needs.** Nothing new for this path, by reading `shim/starlte/fastboot.c` and
`xnu_boot.c`: `fastboot boot KERNEL INITRD --dtb DTB --header-version 2 --cmdline ...` already starts
Linux (`fastboot.c` picks the Linux method when a DTB comes with an arm64 Image; `star_boot_ram` copies the
DTB to 0xae000000 and appends `--cmdline` to its bootargs, puts the initrd at 0xa8000000 (up to 80 MiB) and
sets `/chosen linux,initrd-*`, switches the microSD rails on). Limits: kernel + initrd + DTB below
`max-download-size` 0x04ff0000; the DTB must have bootargs and the initrd placeholders (`mk-env.sh` warns).
The fastboot hook runs after `cat_load()`, whose failure is ignored, so a stock phone without an OS store
should still reach it. **Not verified on the phone** (10, items 2-3): only an XNU payload has gone through
`fastboot boot` so far. Suggested later loader changes (not implemented, not needed for v2): a hidden
catalog entry `installer` reading `installer.image/.initrd/.dtb` from the OS store (like `rescue`), so an
installed phone can start the environment from the menu without a computer-side push; `boot-next
installer`; and dropping the loader's `fastboot reboot recovery` / the recovery menu's TWRP item, which
the installer no longer uses. Engineer OS may host on-phone parts later; for v2 the Linux environment is
the engine.

## 14. One UI 8 on the `starbite-oneui` layout

Written 2026-11-07. One UI 8 (UN1CA 9810, docs/ANDROID-ONEUI.md) is the main Android, on the UFS layout
`starbite-oneui` (section 9.1; S9 #1 has it since 2026-11-07). The installer installs it from a bundle whose
`[ufs] layout` is `starbite-oneui`. Example: `tools/installer/installer-manifest.oneui.example.toml`.

### 14.1 The items

| Example key | `dest` | `action` | File | Made by |
|---|---|---|---|---|
| `oneui-system` | `part:SYSTEM` | `write` | One UI's system, EROFS, raw: 6419857408 B (build of 2026-11-07) | `make_rom -F erofs`, then `tools/android/oneui/sparse2raw.py` |
| `oneui-vendor` | `part:VENDOR` | `write` | the **post-fixed** vendor, EROFS, raw: 697573376 B | `tools/android/oneui/postfix-vendor.sh` only (below) |
| `oneui-odm` | `part:ODM` | `write` | One UI's odm, EROFS, raw: 36864 B | `make_rom` |
| `oneui-data` | `part:A16DATA` | `mkfs-oneui-data` | none | the phone's `mke2fs` (14.3) |

The boot files (`android.kernel`, `android.ramdisk`, `android.cmdline`, `android.dtb` from
`tools/android/oneui/mk-boot-files.py`) go into the UFS store (`store:ufs`, built with `sbstore2.py`), as for
every other OS: no item of their own. The example holds the installer environment, BOOT, the UFS store and the
four items. A full phone set adds the other OSes' items from `installer-manifest.example.toml` unchanged, but
not its `[[layout]]` rows (layout A's numbers; `[ufs] layout` gives the rows) and not its LineageOS items. The
UFS store needs the iOS 16.5 slot in front of it (`store-flash.sh` refuses otherwise), so on a phone that does
not have the slot yet the bundle carries `ios16-slot` too.

**The vendor image must be `postfix-vendor.sh`'s output.** UN1CA's `exynos9810_boot_restore` patch rewrites
both vendor fstabs with /data on USERDATA after every mod (docs/ANDROID-ONEUI.md 2). The vendor.img in UN1CA's
target-files zip, and `make_rom`'s own vendor.img, therefore mount /data on USERDATA, which on a Starbite phone
holds the iOS 16.5 slot and the OS store. **Never put that image in a bundle.** Only the image
`postfix-vendor.sh` rebuilds (fstabs on A16DATA, USERDATA without the `userdata_block_device` label, checked by
the script) goes into `oneui-vendor`. The installer cannot tell the two apart (both are EROFS of a similar
size); the manifest's SHA-256 is the link, so take it from the post-fixed file (S9 #1's: `5572b8b7...`,
docs/ANDROID-ONEUI.md 4).

Install order (`steps.rank`): SYSTEM, VENDOR, ODM, then A16DATA is made, BOOT last. LineageOS keeps its own
order (its A16DATA image, then the fresh /metadata on ODM).

### 14.2 ODM

ODM means two things. On `starbite-a` and `starbite-a16g` it is LineageOS's /metadata (the encryption keys of
A16DATA) and is only ever made fresh (`mkfs-metadata`, `metadata-reset.sh`). On `starbite-oneui` it is One UI's
/odm, an EROFS image the first-stage fstab mounts by name. So:

- `part:ODM` with `action = "write"` loads only when the bundle's `[ufs] layout` is `starbite-oneui`. With
  another layout, or none, it refuses: `ODM (Android /metadata) is only ever made fresh (action =
  "mkfs-metadata"); an ODM image (One UI's /odm, action = "write") needs [ufs] layout = "starbite-oneui" (this
  bundle's is "starbite-a")`.
- `mkfs-metadata` is unchanged (any layout: LineageOS can run on `starbite-oneui` as the fallback). A bundle
  with both `mkfs-oneui-data` and `mkfs-metadata` refuses (One UI has no /metadata).
- The plan's /data + /metadata pairing (renewed together or not at all) applies only to LineageOS: an
  A16DATA image with `mkfs-metadata`. One UI's ODM image is planned on its own: it holds no user data, so an
  ODM that differs is overwritten, an identical one is skipped.
- ODM is written by `tools/android/pixel/part-write.py`, whose allowlist names ODM -> sda20 (`starbite-oneui`
  keeps VENDOR at sda19 and ODM at sda20; `PART_WRITE_PY` in `sbinst/common.py` now lists ODM). The step-1
  backup still takes ODM.

### 14.3 `mkfs-oneui-data`: One UI's /data on A16DATA

One UI's fstab says `length=-20480` for /data: the file system ends 5 blocks of 4 KiB before the partition's end.
The step (`steps.py mkfs_oneui_data`):

1. The size of A16DATA in 4 KiB blocks: in a real run the phone's own `blockdev --getsize64` on the node; in a
   dry run the planned table's A16DATA (the GPT step of the same run may create it). File system blocks =
   partition blocks - 5. On a 15616000-block UFS: 2825467 - 5 = 2825462, as on S9 #1.
2. Whatever is mounted from the node or its by-name link is unmounted (checked again).
3. `mke2fs -t ext4 -F -L data -b 4096 /dev/block/sda28 2825462`, by the phone's own mke2fs: the installer
   environment's e2fsprogs with its `/etc/mke2fs.conf` (an ext4 feature set Android's 4.9 kernel mounts,
   checked when `mk-maint.sh` builds it), or TWRP's with `--transport twrp`. A phone without `mke2fs` stops the
   step before `mke2fs` would run.
4. Verified: `blkid` says `TYPE="ext4"` and `LABEL="data"` (not PARTLABEL), and `dumpe2fs -h` (or `tune2fs -l`)
   says `Block count` = partition blocks - 5 and `Block size` 4096. Anything else stops the run (`STOPPED`); a
   re-run makes the file system again.

A dry run prints the unmount, the exact `mke2fs` line and the check, and runs nothing. The item is always user
data (`data` is forced on) and is never "identical". The plan:

| A16DATA on the phone | plan |
|---|---|
| blank (first and last MiB zero) | `write` |
| moved or resized by the GPT step of this install (`--repartition`) | `write`, no `--replace` needed |
| anything else (One UI's data, LineageOS's f2fs, stale bytes) | `keep`, "has data: kept (--replace oneui-data overwrites it)" |
| the same, with `--replace oneui-data` or `--replace-all` | `REPLACE` |
| made and verified by this installer earlier | `keep`, not read again |
| made by this installer, interrupted before the check | `resume`: made again |
| no A16DATA partition | `BLOCKED` |

The no-password check (section 6) lists One UI 8 with the journal check of Android 17.

### 14.4 Tests

- `tests/test_oneui.py` (23 tests): the manifest (the action accepted only on `part:A16DATA`, no file, `data`
  forced; an ODM image only with `starbite-oneui`, refused with `starbite-a`, `starbite-a16g` and without a
  layout; `mkfs-metadata` unchanged; the two mkfs actions not in one bundle; RECOVERY still refused; the example
  parses with tomllib and the fallback parser, and its order is SYSTEM, VENDOR, ODM, A16DATA, BOOT; LineageOS's
  order and pairing unchanged), and the A16DATA step against a phone held in memory that answers blockdev,
  blkid, dumpe2fs, mke2fs and umount: plan and classification (every row of the table above), the dry-run
  lines, the block count taken from the planned table before a repartition, the real step and its check, a
  file system that fills the partition stopped by the block-count check, a phone without mke2fs.
- `rehearse.sh` section 16 (fake phones): the One UI bundle (`fakes/mkbundle.py --oneui`) verified; an ODM image
  in a `starbite-a` bundle refused; a layout-A phone: the GPT step BLOCKED without `--repartition`, a dry run with
  it (old and new tables, the exact `mke2fs` line, the order, the phone byte-identical), the rehearsal
  (repartition, SYSTEM/VENDOR/ODM images read back, A16DATA's ext4 "data" of 2825462 blocks, protected areas
  untouched, the GPT equal to `starbite-oneui`), a re-run with nothing to do; a phone already on
  `starbite-oneui` without a journal keeps its /data, and with `--replace oneui-data` the plan makes it new.
  The fake `mke2fs` now reads its options (label, block size, block count) and the fakes have `dumpe2fs` and
  `tune2fs -l`.

### 14.5 Not done, and what needs the real phone

- **The bundle builder (section 12) still makes LineageOS bundles** for `ufslayout.ACTIVE` (`starbite-a`): a One
  UI bundle is put together from the example, with the images from the local One UI build. `--from-phone` never
  reads ODM or A16DATA (`bundler.NEVER`). `ACTIVE` is unchanged.
- **Repartitioning a layout-A phone to `starbite-oneui` shrinks WINDOWS** (25.84 to 22 GiB): its NTFS must be
  shrunk with `ntfsresize` first (the layout's notes); the installer does not do that. Without a `windows` item
  the partition is not written, and an NTFS larger than it would be broken.
- A partial bundle such as the example makes the plan warn that the other menu entries "will not boot from this
  bundle" (section 3 `catalog`): they are not in it; what is on the phone stays.
- **Needs the real phone:** the environment's `mke2fs` making One UI's /data, and One UI booting on it (S9 #1's
  /data was made by hand, docs/ANDROID-ONEUI.md 4); `blockdev --getsize64`, `blkid` and `dumpe2fs -h` in the
  environment as the step reads them; `part-write.py` writing the 36864-byte ODM image (one short chunk) and
  the 6 GiB system over USB networking; a full install from a stock phone with the One UI set.

## 15. A second phone on the same computer (S9 #2, 2026-11-15)

Written 2026-11-15 for the first real install: a stock S9 (stock firmware and GPT, bootloader unlocked) while
S9 #1 stays connected to the same Mac. Step by step for the phone: the shared notes' `installer-s9b-runbook.md`.

**`--serial SERIAL`** (required with `--real`; `adb devices` shows it; S9 #1's loader reports the same
`androidboot.serialno` in fastboot). `main()` puts it in `ANDROID_SERIAL` (adb, fastboot and every tool the installer
runs honour it), and:
- every `adb` and `fastboot` call of the installer names it (`adb -s`, `fastboot -s`); `detect()` ignores phones with
  another serial (without `--serial`, two phones still refuse, and the message names the option);
- **Download mode has no serial on USB, and Heimdall has no device selector** (it flashes the first Download-mode
  device). With a serial, one Download-mode device is taken as this phone only if the serial is not visible
  elsewhere; two refuse. Right before the flash `Phone.download_alone()` wants exactly one 04e8:685d on USB and no
  other phone at all (no adb or fastboot device, no other 04e8 / 18d1 USB device): "unplug every other phone". The
  dry run shows the same check as a warning;
- the environment is started with `fastboot -s SERIAL boot ... sb.serial=SERIAL`, and once ssh answers on
  172.16.42.1 its `getprop ro.boot.serialno` (= `sb.serial`) must be SERIAL: another phone's Starbite Maintenance or
  installer environment on the same address is refused, before and after the start;
- Heimdall is the project's own build (`build/heimdall-src/build/bin/heimdall`, v2.2.2, this checkout or the main
  one; `SBINST_HEIMDALL` overrides; rehearsals use the fake on PATH).

**Two dry-run gates.** (A) New: a dry run with the phone in Download mode reads and checks the PIT (read only) and
records `download:<manifest sha256>` in the phone's journal (phone id from the serial); `--real` (and a rehearsal
with `--serial`) refuses the BOOT flash without it. (B) Unchanged: the full dry run in the environment before
any storage write. On a stock phone BOOT is still the first write and comes before the backup: Heimdall cannot
read partitions, and the environment needs the loader in BOOT. Its rollback is the firmware's `boot.img`.

**Backups, verified twice.** Step 1 already compared each partition's SHA-256 on the phone with the local copy
(`part-backup.sh`, raw regions in the installer). Now the whole backup directory is read back on the computer at
the end of the step (`check_backup_dir`: every file in `backups.txt` hashed again, `.img.gz` decompressed, and every
required name present: the PROTECTED list, BOOT, the GPT head and tail, sdb-sde); a mismatch is `STOPPED` before
anything is written. `--backup-only` runs just this step (no dry run needed: it writes only to the computer);
`--check-backup DIR` repeats the check later, e.g. on a copy.

**A bundle without `[ufs]`** (no repartitioning): BOOT, the environment and the card store only. The GPT is only
checked, USERDATA (stock Android's /data) is never touched. `tools/installer/mk-first-bundle.py` makes it: BOOT from
S9 #1's BOOT and a loader Image (`tools/store/boot-card.py`: + `sbstore=card`), padded to 4 KiB; the environment
from `build/maint/store` (`maint.image/.initrd/.dtb`); a fresh `--medium sd64 --home` card store with only the kept
catalog rows (RAM-only OSes; default `starbite-next-gpu`, `engineer`, `utility`) and their files from a pull of S9
#1's store, a fresh `sbos-state` and crash log. Never S9 #1's settings, crash log, Starbite OS state, `wifinet` or
photos. The sd64 store area is 64 MiB: those three take 35.8 MiB; Starbite Maintenance (36 MiB) and Starbite OS's
64 MiB update slots and photo store do not fit.

**`store-flash-sd.sh` and home stores.** It refused every home store (SBSTHOME), because S9 #1's sd128 home store is
a compact image. Now it refuses only a compact one (first data block past 7), on the computer and on the card; an
sd64 home store (data from block 7, laid out as on the card) is written as any other card store.

**Tests.** `rehearse.sh` section 17 (45 checks; `ONLY_S9B=1` runs it alone): two phones and no `--serial` refused;
S9 #1 ignored; `adb -s SERIAL reboot download`; both phones in Download mode refused; the flash refused without
the Download-mode dry run and while S9 #1 is still plugged in (BOOT unchanged); the flash with S9 #1 unplugged;
`sb.serial=` on the environment's command line; `--backup-only` with S9 #1 back on USB (24 files checked);
`--check-backup` and a flipped EFS bit caught; an environment with another serial refused; the dry run (byte
identical) and the install (card MBR, exFAT, card store read back, BOOT skipped as identical); protected areas, both
GPT copies, USERDATA and RECOVERY unchanged; a second run writes nothing. `rehearse-bundle.sh BUNDLE` runs one real
bundle through the same steps on a fake stock S9. Run 2026-11-15 (macOS): unit tests 137 OK; `rehearse.sh` 220
passed, 0 failed; `rehearse-bundle.sh build/installer/bundle-s9b`: every step passed.
