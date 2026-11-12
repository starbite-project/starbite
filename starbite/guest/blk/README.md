# sbblk: the guest's block devices

Milestone L4's transport (docs/STARBITE-LINUX-APPS.md 6): Starbite OS (the host, the four small cores) serves
block devices to the guest Linux on the big cores over the shared region. The contract is `../sbshm.h`,
section "v2: sbblk": a device table, a request ring (guest produces), a completion ring (host produces), a 4 MiB
data window of 64 slots of 64 KiB, the two doorbells, no message-ring use.

| device       | store entry (phone)  | file (QEMU)  | what                                  |
|--------------|----------------------|--------------|---------------------------------------|
| `/dev/sbblk0`| `guest-base.erofs`   | `BASE=`      | the base image, EROFS, read-only      |
| `/dev/sbblk1`| `guest-overlay.img`  | `OVERLAY=`   | the overlay, ext4 (4 KiB blocks), read-write: upper + work |
| `/dev/sbblk2`| `guest-repo.erofs`   | `REPO=`      | the package repo, EROFS, read-only, at `/repo` |

## The pieces

- **Guest**: `../linux/sbguest.c`, a blk-mq driver in the existing module. One tag set of 64 tags shared by
  the disks, so at most 64 requests are in flight: each takes one data slot from a bitmap (the request id is
  the slot plus a per-slot generation, so a stale or doubled completion is caught) and the rings cannot
  overflow. `max_hw_sectors` = 128 (one slot): the block layer splits. Logical block size 4096: every request
  is whole 4 KiB blocks. Submit: payload into the slot, the ring entry, `dmb`, `req_head`, then the host's
  doorbell (`GICD_ISPENDR`, once per batch: `bd->last` / `commit_rqs`). Completions are reaped by one work item
  (the completion ring's single consumer; the 64 KiB copies run in process context, not in the interrupt),
  kicked by the doorbell interrupt and by a timer: every jiffy while requests are in flight when there is no
  doorbell (`db_to_guest` or `db_to_host` 0, or no interrupt), every 50 ms as a safety net otherwise.
  `REQ_OP_FLUSH` -> `SBG_BLK_FLUSH` (the write-cache feature is set on writable devices only). The host's
  errno goes through `errno_to_blk_status` (`-ENOSPC` -> `BLK_STS_NOSPC`, the rest I/O errors). A request the
  host does not answer is never taken back (its slot is the host's): the timeout handler logs, rings again and
  keeps waiting.
- **Host**: `../host/sbghost.c` `blk_serve()`, called from `sbg_poll()`; the platform gives
  `sbg_os.blk_io(dev, op, sector, count, buf)`. Per request: copy the entry once, check it against `cfg.blk`
  (device present, op, slot, count 1..128, range, no write to a read-only device), run `blk_io` on the slot,
  write the completion, `dmb`, `req_tail`, `done_head`; one guest doorbell per poll. Bound per poll:
  `cfg.blk_per_poll` requests (default 8 = 512 KiB) and no new request after `cfg.blk_poll_ms` (default 4 ms)
  in that poll. The host keeps its own ring indices (it never trusts what the guest could have written into the
  region) and stops serving if `req_head` runs more than a ring ahead. Nothing changes when `cfg.v2` is 0.
- **QEMU**: `../sbsim/sbsim.c` opens the files named in the manifest page (`struct blkman` at store + 0x200)
  through semihosting and serves them; `../run-desktop.sh` builds that store and runs it.
- **Root switch**: `../initramfs/init`, `sbguest.mode=desktop`: sbblk0 (EROFS) + sbblk1 (ext4; tmpfs with a
  kernel-log warning when it is absent or does not mount) -> overlay -> `/repo` from sbblk2 -> `switch_root`
  to `/sbin/sbinit`. The two layers stay reachable in the new root at `/.sbroot/base` and `/.sbroot/rw`, so a
  shutdown can `mount -o remount,ro /.sbroot/rw` after `/` and leave the ext4 journal clean.

## Tests (QEMU)

    limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/blk/run-tests.sh [a b c d e f]

`mk-test-images.sh` builds a stand-in base image (busybox, `test-sbinit` as `/sbin/sbinit`, 248 files with their
md5 list), an empty overlay and a repo image in `/mnt/build/sbguest2/blk/img`. `test-sbinit` runs what
`sbblk.test=` names and writes one `sbblk-test: PASS|FAIL` line per test to the kernel log, then
`SBBLK-TEST-DONE`; sbsim (flag `BLKTEST` = 64) stops the guest on that line and fails the run on a FAIL line.
a read, b write + persistence over two QEMU runs, c throughput, d parallel stress, e tmpfs fallback, f the same
without doorbells.

Results of 2026-11-11 (QEMU TCG in the Lima VM, other builds running beside it: the speeds are indicative only):

| test | result |
|------|--------|
| a read | PASS: 248 files match their md5 list, the list's md5 equals the host's; a write to sbblk0 is refused; the repo mounts |
| b persist | PASS: the second QEMU run reads run 1's 4 MiB file; `debugfs` on the overlay file gives the same md5 |
| c speed | PASS: 200 MiB read from sbblk0 at 25 MiB/s; 100 MiB written to sbblk1 (ext4, fsync) at 12 MiB/s |
| d stress | PASS: 4 readers + 3 writers + 1 raw reader, 6 rounds, 1755 MiB read and 231 MiB written, every md5 right, no block error, no hang; `e2fsck -fn` of the overlay file exits 0 |
| e fallback | PASS: with no sbblk1, and with an sbblk1 that is not ext4: tmpfs upper, the warning in the kernel log |
| f no doorbells | PASS: the read and persist-write checks with both sides polling |

The v1 flow is unchanged: `run-qemu.sh patched` (no block devices, contract v1) passes 2/2 with this sbsim, sbghost
and sbguest.ko. In QEMU one poll's block work was seen to take up to about 150 ms (a semihosting call or the
vCPU thread descheduled on the busy VM); the 4 ms bound only decides when no further request is started.

## The phone: the three devices are FILES on the microSD card (2026-11-11, the plan in force)

The OS store moved to the card (docs/STORE-ON-CARD.md) and UFS USERDATA becomes iOS 27's root: nothing may write
UFS, and the store's data area on the card has no room for a root. So the images are files on the card's exFAT
partition p1 (label STARBITE), and Starbite OS serves them by card sectors:

| device | file on p1 | |
|---|---|---|
| `/dev/sbblk0` | `/starbite/linux/base.erofs` | read-only |
| `/dev/sbblk1` | `/starbite/linux/overlay.img` | read-write: ext4, 4 KiB blocks, `/upper` and `/work` |
| `/dev/sbblk2` | `/starbite/linux/repo.erofs` | read-only, optional |

`guest-card.patch` (against the toolkit tip c29ca0d; it also applies after `next/next-linux.patch`'s guest.c
hunks, on d8a69de and on c29ca0d) holds:

- **`starbite/libsb/fence.c`, `sb.h`**: a second standing grant, separate from the volatile one that
  `kern/slot.c` holds: `sb_card_grant(first, count, name)` / `sb_write_card(lba, n, src)` /
  `sb_card_grant_clear()`, in 4 KiB blocks of the card, any size. The fence does not take the caller's word for
  where the file is: it reads the MBR and the exFAT boot sector itself and grants only a range wholly inside the
  **cluster heap of the exFAT partition (type 0x07)**. Refused: anything touching another partition (p2, the OS
  store), crossing p1's end, the partition table, the exFAT boot region, the FAT, the card's live OS store, a
  wrapping range. Writes: inside the grant only, through `sb_blk_write(SB_MED_SD)` under `fence_active` (the
  card driver refuses otherwise), honouring the storage hold and the dry run; the grant remembers the card's CID
  and is void when another card is in the slot. No UFS call anywhere on this path.
- **`starbite/guest/sbos/guest-card.h`** (new; all the code) and **two small hooks in `guest.c`** (the include
  + `.blk_io = gcard_io` at the shim, `gcard_offer();` after `cfg_init();` in `load_and_start`). No edit to
  `cfg_init`: the devices reach sbghost through the new `sbg_set_blk()` (this tree's `host/sbghost.{c,h}`), which
  also selects contract v2 when a device is present. With next-linux.patch in, only the `GUEST_WL` guest gets them.
- At each guest start `gcard_offer()` mounts p1 read-only with the Utility OS's exFAT reader, walks to each file
  and resolves it to one run of sectors.

**The contiguity rule.** A file is offered only if (1) it is a file of at least 4 KiB whose clusters lie in the
volume, (2) it is one run: its directory entry has **NoFatChain**, or its **FAT chain is read and every link is
"the next cluster", with the end mark at exactly the file's last cluster**, and (3) the run lies inside the MBR's
exFAT partition; the three runs may not share sectors. Otherwise the device is not offered and the log says why
(`guest: card: /starbite/linux/overlay.img not offered: the file is fragmented ...`). No base: no devices at all.
No overlay: the guest's init uses its tmpfs upper layer and warns.

**Writes** need all of: `GUEST_CARD_WRITE` (default 1; forced to 0 when guest.c is compiled with
`SBOS_SELFTEST_NEXT_LINUX`), **not a self-test image**, the overlay's run starting on a 4 KiB boundary of the
card, and the fence's grant for exactly that run. Then every write is whole 4 KiB blocks through `sb_write_card`.

**The self-test rule.** `starbite-next-linuxtest` (and every self-test image) writes NOTHING on any medium, the
fenced file included: mounting ext4 read-write already writes its superblock and journal. There the overlay is
not offered; the guest runs on the tmpfs upper and the test checks reads only (base mounted, repo mounted, a
file of base read). How it is enforced: guest.c is compiled once for all Next images and only uitest.c gets
`-DSBOS_SELFTEST_NEXT_LINUX`, so the working condition is `sbos_selftest_enabled` (kern.h; the constant uitest.c
defines at compile time from `SBOS_SELFTEST_ON`, 1 in every self-test image), checked in `gcard_offer()` before
any grant is asked for. The `#ifdef SBOS_SELFTEST_NEXT_LINUX` in guest-card.h covers a build that compiles
guest.c with the flag. Only the normal image (`starbite-next`) asks for the grant.

**Flush** = the card's own completion: libsb's `sd_write` returns only when the card released busy and CMD13
reports the transfer state, so `SBG_BLK_FLUSH` answers 0 with nothing pending.

**Timing.** The card reads at about 10 MB/s (25 MHz, 4 bit): a 64 KiB request is about 6 ms, so with the 4 ms
bound a poll serves one request and returns to the UI; polls follow each other on the guest's doorbell. Expect
single-digit MB/s until the 50 MHz mode exists. Not measured on the phone.

### Host test

    limactl shell starbite -- /Users/dev/project-starbite/starbite/guest/blk/card-test.sh

Extracts c29ca0d, applies the patch, builds libsb's real fence.c, the real exFAT reader and guest-card.h's
resolver against a fake card (`card-test-image.py`: MBR, p1 exFAT with files in every shape, p2 type 0xDA) and a
fake UFS that counts calls. 2026-11-11:

    PASS card-test: resolver (exFAT files to one run each): 19 checks
    PASS card-test: fence (card-file grant, writes inside only, p2 untouched, UFS calls 0): 36 checks
    PASS card-test: unaligned card (reads offered, overlay not): 5 checks
    PASS card-test: 60 checks, 0 failed

libsb's own unit tests (`starbite/tools/unit-test.c`, the engineer build's line) with the patched fence:
567 checks, 0 failed. guest.c with the patch compiles (`-fsyntax-only -Werror`) in four variants (default,
`GUEST_CARD_WRITE=0`, `SBOS_SELFTEST_NEXT_LINUX`, `GUEST_QEMU`). `card-check.py` (below), written separately
from the specification, finds the same runs on the fake card.

### Placing the files (the main session, the phone in TWRP)

1. In the VM: `card-files.sh make OUT BASE.erofs REPO.erofs` (the rootfs session's images; `OVERLAY_SIZE=4G` by
   default). It makes `OUT/overlay.img` (ext4 `-b 4096`, `/upper` + `/work`, e2fsck clean) and `OUT/SHA256SUMS`.
   The files must be reachable from the Mac for adb (copy OUT out of the VM, or to the flash drive: 4 GiB+).
2. On the Mac, phone in TWRP, card in: `card-files.sh push OUT`. It mounts p1 (`twrp mount /external_sd`),
   stops unless that is `mmcblk0p1`, removes old copies of the three files, checks the free space, pushes
   overlay.img, base.erofs, repo.erofs one at a time, compares `sha256sum` on the phone with SHA256SUMS,
   unmounts p1, then runs the check.
3. `card-files.sh check OUT` (also on its own, read-only): `card-check.py --adb --verify OUT` reads the MBR,
   the boot sector, the two folders and the files' FAT entries with `dd` over adb and prints per file
   `PASS ... one run, NoFatChain | FAT chain verified, sectors S + N, starts on a 4 KiB boundary, SHA-256 of the
   run = the file`, or FAIL with the reason. It needs both base.erofs and overlay.img to pass.
4. FAIL FRAGMENTED: the card's free space is in pieces. Delete the three files, make room (the user's files on
   the card are the user's call) or use a smaller OVERLAY_SIZE, push again. FAIL "not on a 4 KiB boundary" for
   overlay.img: this card's exFAT layout is odd; stop and report (reformatting p1 is the user's call).
5. Later changes on a computer (copying files around on the card) can fragment nothing that already exists, but
   deleting and re-copying one of the three files can: run the check again after any such change. The phone
   re-resolves at every guest start and refuses a file that is no longer one run.

### Not done / to know

- Nothing ran on the phone. The guest.c hooks are compiled, not linked into an image here (the Next build is the
  other session's).
- The resolver was not run against a volume written by a real exFAT driver: the VM's kernel has none. The check
  on the real card (step 3, with `--verify`) is that proof, and it uses different code from the phone's.
- The phone's resolver cannot see a file's ValidDataLength (the exFAT reader does not keep it); card-check.py
  refuses a file whose valid length is shorter than its size. A pushed file always has both equal.
- Starbite OS's exFAT reader mounts clusters up to 128 KiB (a 128 GB card's default). A bigger cluster: no
  devices, with that reason in the log.
- kern/files.c (Photos, Files, Music) and the guest both use the card driver from the kernel's loop; the guest's
  requests are at most 64 KiB each. Whether a long Files read can delay a poll noticeably is for the phone test.
- Taking the card out while Linux runs: reads and writes fail (the guest sees I/O errors, as with any pulled
  disk); another card in the slot is refused by its CID on both paths.

## The phone, earlier plan (superseded): devices from OS-store entries

`guest-blk.patch` and the rest of this section describe serving from store entries on UFS. It is kept for the
record only: the store moved to the card and UFS is not to be written. Use `guest-card.patch`.


Starbite OS's guest service is `../sbos/guest.c` (another session's file; `guest-blk.patch` here is a proposal
against its version at `d8a69de`, `git apply --check` clean; it compiles with this tree's `host/sbghost.h`,
which has `cfg.v2`, `cfg.blk[]` and `sbg_os.blk_io`).

### What the store gives today (libsb at d8a69de)

- `store_find(name, &e)`: the entry, with `e.lba` (its first 4 KiB block, absolute on the medium), `e.size`,
  `e.medium` (UFS or the card), `e.flags` (bit 0 = volatile).
- **A range read exists**, in two steps rather than as a store call: `sb_blk_read(medium, 0, e.lba + block, n,
  dst)` (`libsb/blkio.c`; UFS `ufs_read_lu_to`, the card `sd_read4k`). `store_load` (the whole entry, CRC) and
  `store_head` (its first block) are the only store-level reads. The patch reads through `sb_blk_read` and
  checks the range against `e.size` itself.
- UFS DMA above 4 GiB never ran on the phone (guest.c's own note), and the data window is at 0x8_E201_0000: the
  patch reads into the bounce window below 4 GiB that guest.c already uses for the load (`G_BOUNCE`, free once
  the guest runs) and word-copies 64 KiB into the slot.
- **A write of part of an entry exists only for small volatile entries**: `store_volatile_grant(name,
  min_bytes, &e)` checks the entry (UFS, volatile flag, whole blocks, overlaps nothing) and gives the write
  fence a standing grant for exactly its blocks; `sb_write_volatile(SB_MED_UFS, lba, n, src)` then writes inside
  it (forced unit access + SYNCHRONIZE CACHE each time, one log line each time).

### What is missing for the overlay

1. **The grant is at most 1024 blocks (4 MiB)** (`sb_volatile_grant`: `count > 1024` refused). An overlay is
   gigabytes. Needed: a grant sized by the entry (a separate limit for a named kind of entry is enough).
2. **There is one grant in the whole OS** (`vg` in fence.c). Starbite OS's state entry (`sbos/kern/slot.c`)
   holds it; taking it for the overlay makes every later state save fail with `SB_W_RANGE`. Needed: a second,
   independent grant (or a small table of grants).
3. **UFS only**: `sb_volatile_grant` refuses the card and `sb_blk_write_sync` returns -1 for it, while the
   store is moving to the microSD card. Needed: the grant and the write for `SB_MED_SD` (`sd_write4k` exists).
4. **Every write is FUA + a cache flush, and logged**: about 16 one-block WRITE(10) commands plus a flush per
   64 KiB request, and a pmsg line per request (pmsg holds 16 KB). Needed: a write inside the grant without the
   flush and without the per-write log line, and a flush call for `SBG_BLK_FLUSH` (`ufs_sync_cache` exists in
   ufs.c, not through the fence). The patch answers a flush with 0 because today every write is already
   flushed.
5. Not required, but it would put the range check in one place: `store_read(const struct store_ent *e,
   uint64_t block, uint64_t n, uintptr_t dst)` next to `store_head`.

A possible shape for 1 to 4, all in libsb (fence.c, store.c, blkio.c), nothing in the guest service:

    /* a second standing grant, for one large read-write entry (the guest's overlay): UFS or the card */
    int store_rw_grant(const char *name, struct store_ent *e);            /* 1 granted */
    int sb_write_rw(unsigned medium, uint64_t lba, uint64_t n, const void *src);   /* inside it, no flush */
    int sb_flush_rw(unsigned medium);                                      /* the device's cache */

No edits were made to libsb or kern for this.

### What the patch does

- `blk_find()` before each load: `guest-base.erofs` and `guest-repo.erofs` become read-only devices (sizes
  rounded down to 4 KiB). `cfg.v2` is set only when the base entry exists, so the app and terminal guests stay
  on contract v1.
- `blk_io()`: whole 4 KiB blocks only (`sector` and `count` multiples of 8, else `-EINVAL`: no
  read-modify-write of the phone's storage, ever); reads as above; a flush is 0.
- **Writes are impossible unless all of these hold**: the build defines `GUEST_BLK_WRITE=1` (default 0: the
  write call is not compiled in); `guest-overlay.img` exists, on UFS, with the volatile flag
  (`sbstore2.py --volatile`); `store_volatile_grant` granted exactly its blocks; and then each write passes
  the fence's own range check in `sb_write_volatile` (and its dry-run switch: a dry run is reported to the
  guest as an I/O error, never as success). In any other case the overlay device is not offered at all and the
  guest's init falls back to a tmpfs upper layer with a warning in its log.
- With libsb as it is, `GUEST_BLK_WRITE=1` must stay off on the phone: the grant would be refused for any
  useful overlay size (point 1) and, if it were small enough to be granted, it would take the state entry's
  grant away (point 2).

### Timing on the phone

guest.c polls every 10 ms while Linux is shown, 50 ms in the background, and at once on the guest's doorbell
(`guest_irq`). One poll serves at most 8 requests (512 KiB) or stops taking new ones after 4 ms, so the UI tick
is held for about one request's I/O beyond that at worst. A 64 KiB read is one READ(10) on UFS plus a 64 KiB
word copy from the bounce window. Raise `cfg.blk_per_poll` / `cfg.blk_poll_ms` for more throughput at the cost
of UI latency; the numbers have to be measured on the phone.

## A note for sbshm.h (not changed here)

The contract counts in 512-byte sectors and does not say that requests are aligned. The guest driver only
sends whole 4 KiB blocks (logical block size 4096) and the phone's `blk_io` refuses anything else, because the
store's media are in 4 KiB blocks and a read-modify-write there is not acceptable. One sentence in the sbblk
section would make that part of the contract: "sector and count are multiples of 8 (4 KiB blocks); a device's
size is a multiple of 4 KiB". sbghost's server itself accepts any 512-byte range.
