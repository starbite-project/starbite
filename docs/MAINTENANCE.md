# Starbite Maintenance (SBMAINT)

Written 2026-11-06. The user: "move away from TWRP, towards our own stuff ... not on the recovery partition but
a similar one of our own which you can also develop and iterate on."

**Starbite Maintenance** is one Linux maintenance environment (the daily Linux 7.1 kernel + its own initramfs,
run from RAM) that replaces TWRP for every maintenance job, is the Starbite installer's environment, and is the
Starbite rescue system. It lives in **a GPT partition of our own, `SBMAINT`** (two container slots, so a bad
update never leaves the phone without it), with the OS store as the fallback. **Nothing in it writes or boots
RECOVERY**; stock recovery stays where it is (and S9 #1 keeps its TWRP there, untouched, until SBMAINT is proven).

**Status: built (2026-11-06) and rehearsed in the Lima VM against its own userspace; NOT run on the phone.**
The loader does not start it yet except through `fastboot boot` (works today) and the proposed catalog entries
(section 4.3, work on today's loader once the store has the files). Section 12 lists what needs the phone.

| | |
|---|---|
| Environment sources | `tools/maint/env/` (`init`, `sb-*`, `getprop`, `memdump.c`, `mke2fs.conf`, `mkfont.py`, `mk-maint.sh`) |
| Container | `tools/maint/sbmaint.py` (format + the loader's slot choice as reference code), `tools/maint/pack.py` (CLI) |
| Host CLI | `tools/maint/sbm` |
| Transport | `tools/maint/transport/adb` = `tools/installer/transport/adb` (one shim for the installer and SBMAINT) |
| Tests | `tools/maint/tests/` (34 host tests), `tools/maint/rehearse.sh` (the VM rehearsal, 35 checks) |
| Build output | `build/maint/` (`sbmaint.img`, `maint.initrd`, `maint.dtb`, `maint-rescue.dtb`, `known_hosts`, `keys/`) |
| Layout proposal | `ufslayout.PROPOSALS['proposal-sbmaint']` in `tools/installer/sbinst/ufslayout.py` (section 4.1) |
| Catalog proposal | `tools/maint/catalog-maint.toml` (hidden entries `maint`, `maint-rescue`; not applied) |

## 1. Quick start

```sh
# build (VM): initramfs, DTBs, the SBMAINT1 container, the store entries, the ssh host key (made once, kept)
limactl shell starbite -- sudo bash /Users/dev/project-starbite/tools/maint/env/mk-maint.sh
python3 tools/maint/pack.py verify build/maint/sbmaint.img

# start it from RAM (writes nothing): the loader's fastboot (Volume Up alone at power-on, or Bixby > USB FASTBOOT)
tools/maint/sbm boot-ram                       # then: sbm --transport maint status
export SB_TRANSPORT=maint                      # every host tool now talks to SBMAINT instead of TWRP
tools/maint/sbm status | shell | logs | backup EFS | write SYSTEM img | store-pull out.img | reboot-to menu
tools/android/pixel/part-write.py SYSTEM system.img          # unchanged tools pick the transport up themselves
tools/maint/sbm run tools/store/store-flash.sh build/store/ufs-store.img    # tools outside tools/android/pixel
tools/maint/sbm update [--real]                # (once the partition exists) the container into the spare slot
```

`SB_TRANSPORT` unset (or `twrp`) keeps TWRP's adb exactly as before: **TWRP stays the default on S9 #1 until
SBMAINT is proven on the phone.**

## 2. TWRP, job by job

| TWRP job today | Tool | With SBMAINT | Rehearsed |
|---|---|---|---|
| Write SYSTEM / VENDOR / ODM / A16DATA | `part-write.py` | `SB_TRANSPORT=maint` (or `sbm write`); `readlink -f by-name/X` gives `/dev/block/sdaN` as in TWRP | yes (zero chunk + pushed chunk, verified) |
| Back up / restore partitions | `part-backup.sh mac\|card\|restore-card` | same; the card's exFAT is at `/external_sd` as in TWRP | `mac` yes; `card` no |
| Fresh `/metadata` | `metadata-reset.sh` | same: SBMAINT's `/etc/mke2fs.conf` makes the ext4 TWRP's mke2fs made (no `orphan_file`, mountable by Android's 4.9) | yes |
| Patch the GSI's system image in place | `block-patch.py` | same; or, new: the 7.1 kernel mounts that ext4 read-write (TWRP's 4.9 could not) | no |
| OS store flash / pull | `store-flash.sh`, `store-pull.sh` | `sbm store-flash` / `sbm store-pull` | yes |
| Card store, card ranges, Windows image | `store-flash-sd.sh`, `card-range-flash*.py`, `write-windows.sh` | `sbm run ...` (same adb subset; `exec-in` streams are reliable over ssh, they were not with TWRP's adbd) | no |
| A shell | `adb shell` | `sbm shell` (ssh, key only) | yes |
| GPT re-read after a table change | `adb reboot recovery` | `blockdev --rereadpt /dev/block/sda; sb-byname` | by-name re-read: unit test |
| Android logs | `collect-logs.sh` | `sbm logs --android` (pstore, MISC, CACHE, ODM work); `/proc/last_kmsg`, `/proc/sec_log`, `/proc/reset_reason` are Samsung-kernel files, empty here: `sbm logs` reads S-Boot's sec_log RAM instead | no |
| Unattended test boot, then the logs | `boot-next.sh` | **still TWRP**: XNU test kernels reset into RECOVERY (sweetreboot) and the tool waits for TWRP. SBMAINT side ready: `sbm reboot-to KEY`, the test OS resets, `sbm logs` reads the rings SBMAINT's DTB leaves intact. Needs the loader's return key (4.2, R7) or test kernels that reset with the mailbox key `maint` | logs decoding: yes |
| Flash BOOT (the loader) | `twrp-flash-boot.sh`, `flash-boot.sh` | the same `dd` over the transport (BOOT is writable in SBMAINT; RECOVERY is read-only and refused) | no |
| Installer, S9 #1's own maintenance | `starbite-install --transport twrp` | the installer's default transport already is this environment (installer mode, section 5) | the installer's rehearsal: 144/144 |

**What still needs TWRP until proven on the phone:** everything, by default. In order of what SBMAINT has to
show first: the start through the loader's fastboot (12.1-12.3), the USB link and ssh (12.4), then the writers
one by one (12.6). `boot-next.sh`'s unattended loop stays on TWRP until R7 or the test kernels' change exists.

## 3. The container: SBMAINT1

One slot. Little endian; a 4096-byte header, then the sections at 4096-aligned offsets (from the slot's first
byte) in this fixed order, each zero-padded to 4096. `tools/maint/sbmaint.py` is the reference implementation.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `SBMAINT1` |
| 8 | 4 | version = 1 |
| 12 | 4 | header_size = 4096 |
| 16 | 4 | seq: generation, >= 1 (of two valid slots, the higher one starts) |
| 20 | 4 | flags = 0 (reserved; ignore unknown bits) |
| 24 | 8 | total: bytes from the slot start to the end of the last section (multiple of 4096) |
| 32 | 8 | build_time (Unix seconds; display only) |
| 40 | 24 | reserved, 0 |
| 64 | 4 x 32 | sections: `{u64 offset; u64 size; u32 crc32; u32 kind; u64 reserved}`; [0] kernel (kind 1, arm64 Image), [1] dtb (kind 2, USB device), [2] initrd (kind 3), [3] dtb-rescue (kind 4, USB host; size 0 = none, then offset 0 and crc 0) |
| 192 | 256 | cmdline: NUL-terminated ASCII, appended to the DTB's `/chosen/bootargs` |
| 448 | 64 | version: NUL-terminated ASCII (`sbmaint 2026-11-06 <git>`), for screens and logs |
| 512 | 4 x 32 | SHA-256 of each section (hosts check these; the loader need not) |
| 640 | | zeros |
| 4092 | 4 | header_crc32 = CRC-32 (zlib/IEEE, as the store and SBRESCUE) of bytes 0..4091 |

Limits: kernel <= 64 MiB, each DTB <= 2 MiB, initrd <= 80 MiB, **total <= 0x04FF0000** (the loader's 80 MiB
download buffer, so it can stage a whole container and check every CRC before it places anything; the same
limit as `fastboot boot`). Today's container: 34.9 MiB (kernel 25.1 MiB, initrd 9.7 MiB, two DTBs).

`pack.py pack|verify|show|extract|store-entries|partition|select`; `verify` makes every check the loader
makes plus SHA-256, the kernel/DTB magics, and the DTB facts (bootargs, initrd placeholders, dr_mode,
whether Linux's ramoops would bind).

## 4. Where it lives, and what the loader must do

### 4.1 The `SBMAINT` partition (proposal for the One UI 8 layout; the coordinator decides)

- **Name `SBMAINT`**, 256 MiB (65536 blocks of 4 KiB) = two 128 MiB slots: **slot A at byte 0, slot B at
  half the partition rounded down to 1 MiB** (`sbmaint.slot_b_offset`). Minimum workable size: 160 MiB
  (two slots of 80 MiB). 256 MiB leaves room for a bigger initramfs or its own kernel.
- **Type GUID `52415453-4942-4554-204d-41494e543031`** (the bytes "STARBITE MAINT01", Samsung-style): Windows
  gives an unknown type no drive letter, and tools can tell it from data. **Attribute bit 0** (platform
  required). Unique GUID derived from the name (`ufslayout.derived_guid`), the same on every phone.
- **Placement in the proposal: the last row (sda29), after A16DATA fixed at 15.75 GiB** (was FILL, 16 GiB, in
  `starbite-a16g`, the sizes the user asked for: Windows ~18 GiB, Android data ~16 GiB). No existing row is
  renumbered, so `part-write.py` (sda18/19/20/28), `preflight.sh` and `metadata-reset.sh` stay valid. Planned
  by the installer's GPT engine on S9 #1's real GPT (test `test_layout_proposal_on_the_real_gpt`): SBMAINT
  15550208..15615743, the 17 protected rows byte-identical, fresh rows WINDOWS, A16DATA, SBMAINT.
- Other choice: SBMAINT before A16DATA (A16DATA stays FILL), which renumbers A16DATA to sda29 and needs the
  tools' sdaN tables changed. Either way the loader finds it **by name**.
- `python3 tools/installer/sbinst/ufslayout.py show proposal-sbmaint`; it is in `PROPOSALS`, not `LAYOUTS`
  or `TARGETS`, so no bundle can lay it out until the coordinator moves it.
- Written by `sbm update` (from SBMAINT itself or from TWRP): the slot the loader does **not** start now gets
  the container with seq = highest + 1; its header block is zeroed first, then the body, then the header,
  then everything is read back. A torn write leaves an invalid slot, which the loader skips: the old one
  still starts (rehearsed). The SBMAINT partition is not in the read-only list.

### 4.2 The loader (shim/starlte; for the loader agent, nothing here is implemented)

**Status (2026-11-06, loader branch `worktree-agent-a1918b867acea2ce7`, built and host-tested, not run on
the phone): R1, R2, R3, R4, R5 and R7 are implemented; R6 and R8 are not.** Details and the differences
(R1's variant is a second prompt, not a held key) in docs/STARBITE-HEALTH.md section 12. The loader keeps
POST slots 202..212 for checks 0..10 (the full POST's new checks are in 240..244).

**R1. Recovery menu item "STARBITE MAINTENANCE"** (after "STARBITE RESCUE MODE"). On select:
`star_maint_ready(&why)` (any source below has a valid candidate: slot headers only, no body read needed);
if none, show "NO MAINTENANCE SYSTEM: <why>" in red and stay; else write the boot-next mailbox key `maint`
(key words first, magic `SBNEXT2!` last, exactly as Rescue does) and reset with INFORM3 0x12345670 + SWRESET.
Optional: holding Volume Up while selecting writes `maint-prev` (the other slot), Volume Down `maint-rescue`.

**R2. Early mailbox keys**, taken where `rec_rescue_take()` takes `rescue` (right after the Bixby window,
before the splash, POST, catalog UI and menu): `maint`, `maint-prev`, `maint-rescue`. Clear the mailbox
**before** starting (a crashing SBMAINT is not retried; `panic=10` then lands in the menu). Bixby held during
that boot cancels the request (recovery menu). Any failure: the recovery menu with the reason in red.
Until R2 exists, the key `maint` reaches `menu.c boot_next()` and starts the catalog entry `maint` (4.3).

**R3. Fastboot**: `fastboot oem maint [prev|rescue]` starts it straight from the partition/store (no download);
`fastboot oem boot-next maint` needs nothing new (the mailbox + R2). `fastboot boot` of the extracted parts
already works on today's loader (`sbm boot-ram`).

**R4. Sources, in order** (`maint` and `maint-rescue`; `maint-prev` reverses the slot order):
1. GPT partition named `SBMAINT` on LU0 (`gpt_find`). For slot A (byte 0) and slot B (`(size/2) & ~0xFFFFF`),
   read the 4096-byte header. Valid if: magic, version 1, header_size 4096, header CRC, seq != 0,
   `total % 4096 == 0`, `4096 <= total <= min(slot size, 0x04FF0000)`, the four section kinds are 1,2,3,4 in
   order, kernel/dtb/initrd sizes > 0, every present section 4096-aligned, not before the previous one's end,
   inside `total`, within its limit; an absent dtb-rescue has offset 0 and crc 0. Try valid slots by seq,
   highest first (A before B on a tie). For the first: `ufs_read_to` the whole `total` into STAGE
   (0x8c000000), check each present section's CRC-32 and the kernel's `ARMd` at +56; on any failure, the next
   slot. `pack.py select IMG [--prev]` and `sbmaint.choose()` are the reference.
2. The OS store via the catalog entry `maint` (or `maint-rescue`) if the catalog has one (4.3): `boot_linux`
   with that entry (its files and cmdline).
3. Nothing: "NO MAINTENANCE SYSTEM: SLOT A <why>, SLOT B <why>, NO MAINT ENTRY IN THE STORE".

**R5. The start**, as `rescue_from_bundle` / `boot_linux` (no POST, no menu):
- kernel to RAM_BASE + text_offset (checked against OS_RD), initrd to OS_RD 0xa8000000, DTB to LINUX_DTB
  0xae000000: the `dtb` section, or for `maint-rescue` the `dtb-rescue` section (the `dtb` one if absent);
- the DTB must have `/chosen/bootargs` and `linux,initrd-start/-end` (fill them, and `starbite,rtc-epoch`);
  append to bootargs: `" " + header.cmdline + " sb.maint=1 sb.maint.src=a|b|store sb.maint.seq=N
  sb.maint.from=menu|next|fastboot"`, plus `" sb.rescue=1"` for `maint-rescue`, plus
  `" sb.serial=<androidboot.serialno>"` (the environment's USB serial string and `getprop ro.boot.serialno`;
  the installer's phone id needs it when it finds SBMAINT already running);
- the microSD rails on (`hw_sd_rails`), as for every Linux;
- screen rows 0-1: "STARBITE MAINTENANCE" / "STARTING SLOT A (SEQ N)", pmsg
  `STARBITE: maintenance from slot a seq N <version>`, log slot 10 = 0x710 (slot A), 0x711 (slot B),
  0x712 (store), 0x713 (fastboot oem maint).
- Leave INFORM3 alone; the mailbox is already cleared.

**R6 (optional). Mailbox key `download`**: INFORM3 0x12345671 + SWRESET from the loader. INFORM3 written from a
mainline kernel did not survive its restart for `recovery` on 2026-11-05; until verified for download, `sbm
reboot-to download` goes through the loader's fastboot (`sb-reboot fastboot`, then `fastboot reboot download`).

**R7 (optional). A return key for unattended test boots**: magic `SBNEXT2R` with two NUL-separated keys
(`KEY\0maint\0`, 24 bytes): the loader starts KEY and re-arms the mailbox with plain `SBNEXT2!` + `maint`
right before the handoff, so the test OS's reset (if it does not overwrite the ring) comes back to SBMAINT,
not to RECOVERY. With it, `boot-next.sh`'s loop no longer needs TWRP.

**R8 (optional). Rescue**: "Starbite Rescue mode" may fall back to the SBMAINT partition in rescue mode (after
the SBRESCUE bundle, before the store's `rescue.*`), so one rescue system covers it all. The BOOT bundle stays
first: it needs no UFS.

What SBMAINT itself writes into the mailbox for its reboots: `menu` (cleared), `maint`, `maint-prev`,
`maint-rescue`, `fastboot`, any catalog key (`tools/linux/reboot-to -w`), and INFORM3 download.

### 4.3 The OS store fallback (phones without the partition, and today's loader)

`pack.py store-entries build/maint/sbmaint.img DIR` gives `maint.image`, `maint.dtb`, `maint.initrd`,
`maint-rescue.dtb`; add them to the store (`sbstore2.py build --import LIVE ... maint.image=DIR/maint.image ...`)
with the two hidden catalog entries in `tools/maint/catalog-maint.toml` (`maint`, `maint-rescue`: files
`maint.image|linux.image`, `maint.dtb` / `maint-rescue.dtb|rescue.dtb`, `maint.initrd`; cmdline
`sb.maint=1 [sb.rescue=1] sb.maint.src=store s9p_decon.panel_mask=2 s9p_decon.geom_log=0`). That works on
today's loader: `reboot-to maint` from any Starbite Linux, `sbm reboot-to maint`, settings `next=maint`,
Bixby > Starbite Recovery > Boot entry... Cost: about 35 MiB of the store (10 MiB without its own kernel).
Not applied here: `tools/store/catalog.toml` and the live store belong to their owners.

## 5. The environment

`/init` (`tools/maint/env/init`), three modes from the command line:

| Mode | When | What |
|---|---|---|
| maint | default (`sb.maint=1`) | USB device: CDC-ECM (or NCM with `sb.maint.usb=ncm`) + CDC-ACM; usb0 172.16.42.1, DHCP gives the computer 172.16.42.2; dropbear on usb0 only, **key only**; the card's exFAT at `/external_sd` (dirty flag: `fsck.exfat -p` first); the status screen; Power held 3 s reboots to the menu |
| installer | `sb.installer=1` (the installer's `fastboot boot`) | the same, the card not mounted; the key the installer appends to the initrd is merged with the built-in ones |
| rescue | `sb.rescue=1` | every UFS disk read-only, then `tools/rescue/init` runs embedded (section 7) |

- **Keys**: `/etc/sbmaint/authorized_keys` (built in: `build/linux/ssh/starbite_ed25519.pub` and the
  installer's `build/installer/env/id_ed25519.pub`) + any appended `root/.ssh/authorized_keys`. The **ssh host
  key** is made once (`build/maint/keys/`, not in git) and baked in; `build/maint/known_hosts` pins it for the
  computer (`HostKeyAlias=sbmaint`), so a distro that also answers on 172.16.42.1 is refused by ssh.
- **Block paths** (`sb-byname`): a real block node `/dev/block/NAME` for every block device (from sysfs `dev`),
  `/dev/block/by-name/PARTNAME` relative links, TWRP's `/dev/block/platform/11120000.ufs/by-name`. Real nodes
  matter: the tools compare `readlink -f by-name/SYSTEM` with `/dev/block/sda18` (the installer environment's
  links into `/dev` would have made every one of them refuse). Run again after a GPT change.
- **Read-only flags** (`sb-protect`): BOTA0-2, EFS, CPEFS, PARAM, UP_PARAM, KEYSTORAGE, DTB, DTBO, RECOVERY,
  RADIO, NAD_REFER, PERSISTENT, MISC, STEADY, DQMDBG and every UFS LUN other than sda (S-Boot's) are
  read-only; `sbm unlock NAME` makes one writable until reboot (logged on the screen), **RECOVERY never**. BOOT,
  the OS partitions, SBMAINT and sda itself (the GPT step) stay writable. A whole-disk write at a protected
  offset is not stopped by this; the tools' allowlists remain the first defence.
- **Reboots** (`sb-reboot`): `menu` (mailbox cleared), `maint`, `maint-prev`, `maint-rescue`, `fastboot`, any
  catalog key, `download`; `recovery`/`twrp` refused. VBUS off first (rescue's host port), card unmounted, sync,
  sysrq b.
- **Status screen** (`sb-status`, fbcon on tty0, every 2 s): mode, version, start source/seq, USB link state,
  IP and the computer's address, ssh sessions, **the current job** (`sb-job`: the transport sets it to
  "tool: command" for every command, `sbm job TEXT` by hand), battery, card, partitions read-only/writable,
  the last log lines. Font: kbd's `setfont` with a 32x64 Terminus (scaled 2x at build time), else busybox
  `loadfont` 16x32, else fbcon's 8x16.
- **`/etc/mke2fs.conf`**: e2fsprogs 1.46's ext4 features (no `orphan_file`, `metadata_csum_seed`,
  `fast_commit` ...), checked at build time.
- Tools: sgdisk gdisk blkid e2fsprogs(+extra) f2fs-tools exfatprogs dosfstools ntfs-3g(+progs) erofs-utils
  squashfs-tools losetup partx sfdisk wipefs lsblk findmnt zstd lz4 xz gzip pigz coreutils dropbear usbutils;
  Alpine's busybox is the shell; Ubuntu's static busybox (as `busybox.static`) only for devmem/udhcpd.
  **Alpine's busybox on purpose**: Ubuntu's busybox-static ash runs its own applets before PATH (found
  2026-11-06), so `mke2fs`, `blkid`, `blockdev` in any script would silently be busybox's; mk-maint.sh refuses
  such a build.
- The kernel cannot **mount** f2fs (CONFIG_F2FS_FS unset in the daily kernel; `mkfs.f2fs`/`fsck.f2fs` work on
  the raw partition) and has no in-kernel NTFS (`ntfs-3g` via FUSE mounts it).

## 6. What the previous boot left in RAM

The maintenance DTB keeps Linux's ramoops driver off the log region (its reserved-memory node's `compatible`
renamed: still reserved, not used), and drops `console=ramoops-1`. So the previous boot's rings and the
boot-next mailbox survive, and the init copies them first thing with `memdump` (mmap of `/dev/mem`, which also
reads the DTB's no-map regions) into `/run/sbmaint/early/`: the ramoops region 0xFED10000 (32 KiB: dmesg zone,
console/pmsg zone, the mailbox at 0xFED17FE0), the loader's log page 0x91FFF000 (POST slots 200-212, slot 10 =
how the last OS started) and S-Boot's sec_log at 0xC0000000 (512 KiB, the DTB's `seclog` reservation).
`sbm logs` pulls them and writes `ramoops-*.txt`, the mailbox state, a decoded POST line, `loader-log.txt`,
`seclog.txt`, plus dmesg, the environment's log, by-name, blkid, mounts.

## 7. Rescue mode

`sb.rescue=1` (the `dtb-rescue` DTB: USB host by default): `sb-protect apply-all`, then `tools/rescue/init` runs
as `/bin/sb-rescue` with `SB_RESCUE_EMBEDDED=1` (no second mount of proc/sys/run). It looks for a USB disk or
card partition labelled `STARBITE-RESCUE` / `SB-RESCUE`, checks `rescue.sha256`, runs `rescue.sh`, exactly as
the standalone rescue ramdisk (`tools/rescue/README.md`). Difference: **Volume Down** switches the port to a USB
device and starts the whole maintenance link (`sb-usb device`: ECM + ssh + ACM), not only a serial shell; Volume
Up goes back to host mode. The small standalone `rescue.initrd` stays for the SBRESCUE bundle in BOOT (BOOT's
free space cannot hold SBMAINT's 35 MiB).

## 8. Building

`tools/maint/env/mk-maint.sh` (VM, root; about 4 minutes): the Alpine 3.24 minirootfs + packages, the trims, the
busybox arrangement, `memdump` (static), the files above, the fonts, the keys and host key, the checks (every tool
present, the shell follows PATH, the real tools win, the ext4 feature set), `maint.initrd` (newc | zstd -19),
`maint.dtb` / `maint-rescue.dtb` (from KDIR's DTBs: ramoops kept, dr_mode checked), the container, the store
entries, `known_hosts`, `SHA256SUMS`, `BUILD-INFO`; copies to `build/maint/`. KDIR defaults to
`~/distros/_kernel-mask` (7.1.0-starbite-4da38c67, the store's `linux.image` since 2026-11-06).
`tools/installer/env/mk-env.sh` now builds the same thing and names it `installer.image/.initrd/.dtb`; the
installer environment's `init`, `sb-byname`, `getprop`, `sb-reboot` are links into `tools/maint/env/`.

## 9. The transport: SB_TRANSPORT

`tools/maint/transport/adb` (= `tools/installer/transport/adb`) is a transparent `adb`: first on PATH, it hands
every call to the next real `adb` unless `SB_TRANSPORT=maint` (or the installer's `SBINST_ENV_UP=1`). In maint
mode it maps adb's CLI onto ssh: `shell` (exit status, stdin, 8-bit clean), `exec-out`, `exec-in` (reliable
over ssh), `push`/`pull` (files and directories), `get-state` ("recovery" only when the environment answers),
`devices`, `wait-for-*`, `reboot [bootloader|download]` (`reboot recovery` refused). Every command is prefixed
with a guard (the far side must have `/etc/sbmaint-release`, else exit 97 and nothing runs) and the job line.
Settings: `SB_MAINT_HOST`, `SB_MAINT_KEY`, `SB_MAINT_KNOWN_HOSTS`, `SB_JOB`, `SB_MAINT_PORT` (rehearsals).

How tools pick it up: `part-write.py`, `part-backup.sh`, `metadata-reset.sh`, `block-patch.py` and
`preflight.sh` put the transport first on PATH themselves when `SB_TRANSPORT` is not `twrp` (three lines each).
Every other tool runs unchanged under `sbm run TOOL ...` (or with `tools/maint/transport` first on PATH, which
is harmless while `SB_TRANSPORT` is unset). Suggested for the owners of `tools/store/*.sh`,
`tools/android/collect-logs.sh` and the others: the same three lines.

## 10. sbm

`sbm [--transport maint|twrp|auto] COMMAND`: `status`, `shell [CMD]`, `push`, `pull`, `backup PART [OUTDIR]
[--card TAG]`, `write PART IMG`, `store-flash IMG`, `store-pull OUT`, `logs [TAG] [--android]`,
`reboot-to KEY|menu|maint|maint-prev|maint-rescue|fastboot|download` (KEY must be a catalog key: never a
payload name), `job [TEXT]`, `unlock PART`, `run CMD...`, `boot-ram [--rescue]`, `update [--real]`,
`verify-image`. Default transport: `$SB_TRANSPORT`, else `twrp`. `status` also names the computer's USB
interface whose MAC is SBMAINT's host side (02:53:39:00:02:02), and a loader in fastboot.

The development loop: edit `tools/maint/env/*` -> `mk-maint.sh` -> `sbm boot-ram` (RAM only) -> test -> `sbm
update --real` when good (the other slot keeps the previous one; `sb-reboot maint-prev` starts it).

## 11. Tests (no phone)

- `python3 -m unittest discover -s tools/maint/tests` (34): containers (round trip, byte layout, every
  corruption, DTB checks), the two-slot choice and the update plan (torn writes never lose the booting slot), the
  ramoops decoder; by-name from S9 #1's real GPT backup (every partition, part-write.py's ALLOW, preflight.sh's
  table, ODM, a re-read, a USB disk with a clashing name), sb-protect, sb-reboot's mailbox words, the layout
  proposal through the GPT engine; the transport against a fake ssh (hand-off to the real adb without loops,
  shell/exec-out/exec-in, push/pull of directories, the guard, the host-key pin, reboots) and `part-write.py` +
  `part-backup.sh` unchanged through `SB_TRANSPORT=maint`. Plus the real build when `build/maint/` has one.
- `limactl shell starbite -- sudo bash .../tools/maint/rehearse.sh` (35 checks): the built initramfs in a
  chroot, its own dropbear on 127.0.0.1:2222 with the built-in host key, the unchanged host tools over ssh:
  login and pin, by-name, `part-write.py` (dry run and real), `part-backup.sh`, `metadata-reset.sh` (ext4
  features), `store-flash.sh` + `store-pull.sh` (byte-identical), `sbm update` A then B then a torn B (falls
  back to A) then the rewrite, push/pull, job, unlock, `sbm logs` decoding, refusals (recovery, a payload name
  as key, a distro without the marker, a different host key). Partitions are sparse files, `/proc` and `/sys`
  are fakes, and every reboot or memory tool is a recording stub: nothing can reboot or touch the VM.
- `tools/installer/rehearse.sh`: 144/144 with the extended shim; installer unit tests 111/111.

## 12. What needs the phone (in this order), and risks

1. `sbm boot-ram`: the loader's `fastboot boot` of a **Linux** kernel + initrd + DTB (only XNU has gone through
   it so far, docs/INSTALLER.md 10 item 3); the bootargs append and the initrd placeholders.
2. The init on the phone: by-name from the real UFS (`sb-byname` against real sysfs), the read-only flags, the
   memdump reads (the ramoops region mapped normal, sec_log no-map), the console font (setfont 32x64 or
   loadfont 16x32; readability on the panel), the panel with `s9p_decon.panel_mask=2`.
3. USB: dwc3 as a device after the loader's own USB use; ECM on macOS 27 (a new interface, 172.16.42.2 by DHCP,
   MAC 02:53:39:00:02:02); NCM as the fallback (`sb.maint.usb=ncm`).
4. dropbear with the baked host key and keys (pinned `known_hosts`); `sbm status`.
5. `sb-reboot menu` (mailbox cleared) and `sb-reboot maint` / KEY from a mainline kernel (the mailbox survived
   for `reboot-to` from the distros; INFORM3 download did not for recovery: use the fastboot route).
6. The writers on the real phone, smallest risk first: `sbm backup EFS`, `sbm store-pull`, then
   `part-write.py --dry-run`, `store-flash.sh` (with a fresh store-pull first), then real writes.
7. The store entries + the catalog proposal (4.3) on today's loader: `sbm reboot-to maint` via the catalog.
8. Rescue mode: the host port (docs/RESCUE-USB-HOST.md, untested), Volume Down to the maintenance link (the
   role switch from host to device, untested).
9. After the loader implements R1-R5 and the layout gets SBMAINT: `sbm update --real`, Bixby > Starbite
   Maintenance, `maint-prev`.

Risks: the Linux `fastboot boot` path (item 1); fbcon/console size on the real panel; Ubuntu's static busybox in
the daily `linux.initrd` has the same applets-before-PATH behaviour (its `e2fsck`, `losetup` calls may be
busybox's: worth a look by its owner); `blockdev --rereadpt` with UFS in use (nothing mounts sda here).
