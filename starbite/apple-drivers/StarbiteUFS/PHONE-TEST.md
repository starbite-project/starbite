# StarbiteUFS: first phone test (milestone M4)

**Question:** does IOStorageFamily publish IOMedia for the Starbite-owned UFS range, and only for it, read-only,
and can the root be mounted through it, with no foreign LBA touched?

House rules: one phone (S9 #1); I run every host step, you only press buttons; dry run and verified backup first;
boot by catalog key; `store-flash && boot` chained; the image arms its own dead-man; nothing is written to UFS
(no `sbufsrw`, no `starbite,writes-allowed`: the driver cannot write even if asked). Before each boot I say what
changed, what to expect, the roadblock and the plan; after it, what happened.

## Before the phone (host)

1. `sh starbite/apple-drivers/StarbiteUFS/test/run.sh` passes; `kcheck.py` on the .cpp and the three hw/*.c is OK.
2. In TWRP, read-only, LU 0's GPT head (24 KiB) and the first block of USERDATA:
   `adb shell dd if=/dev/block/sda bs=4096 count=6 > lu0-head.bin`,
   `adb shell dd if=/dev/block/sda25 bs=4096 count=1 | xxd -s 1024 -l 2` (expect `482b` "H+" or `4858` "HX").
3. `tools/sbufs-fence.py --gpt lu0-head.bin --allow USERDATA:root:19200 --out fence/` (19200 = `ufsrootlen` of
   the iOS 27 root); check `fence.txt`: one allow line `USERDATA lba 1698432 +4915200 flags 2`, deny lines for
   GPT-HEAD, GPT-TAIL and every other partition (EFS, CPEFS, PARAM, KEYSTORAGE, PERSISTENT, STEADY, BOTA*, DTB,
   DTBO, BOOT, RECOVERY, RADIO, ...). The two .bin files go into the ADT's `/arm-io/ufs` (INTEGRATION.md).
4. The catalog test row (hidden): `method = "xnu"`, `wdt = 82`, `wdt_feed = "xnu"` (the loader arms cluster 1's
   watchdog and appends `sbwdt=82`), **not** `no_launch` (a no_launch row gets no watchdog). Store backup
   verified; `store-flash.sh ... && boot-next <key>` chained.

## Dead-man (independent of XNU)

`sbstop=` (kernel 0423+) makes this a test boot: the loader's watchdog is **never fed**, so cluster 1's watchdog
resets the phone **82 s after the hand-off whatever XNU does** (hang, deadlock, IOKit wait for root, stuck I/O);
INFORM3 = recovery is written at boot, so that reset, any panic, and the stop itself all land in TWRP. The
driver's own stop (`sbstop=ufs` / `ufsroot`) normally comes first and dumps the console tail into pmsg.
Check in pmsg that the line
`STARBITE HEALTH: sbstop=ufs: test boot (0423): the watchdog is never fed (it resets in sbwdt=82 s ...)`
has **no** `WARNING: the loader armed no watchdog`. If it has, the run was protected only by the stop itself:
fix the catalog row before the next boot.

## M4a: storage only (no root), polled

Boot-args (iOS 26 identity, like tools/cmdline/apple-m1.txt, minus the card root):
```
debug=0xc serial=3 cpus=4 -v wdt=-1 a5fb_keep_on=1 sweetfb_native=1 sweetfb_scale=1 sweetfb_lw=828 sweetfb_lh=1792 vm_compression_limit=196608 sbufsirq=0 sbstop=ufs
```
No `rd=`: XNU waits for a root forever, which is fine; the stop fires once the IOMedia tree has settled (3 s
after the last IOMedia). Expected in pmsg (`/sys/fs/pstore/pmsg-ramoops-0`), in this order (numbers from the
phone where known, `..` = to be seen):
```
StarbiteUFS: fence: 1 allowed ranges, 16 denied extents; writes off (ADT 0, sbufsrw=0)
StarbiteUFS: HCI at 0x11120000, vendor HCI at 0x11121100, UFSP mapped (read only)
StarbiteUFS: DMA area 0x.., 413696 bytes (16 KiB pages: 425984), write-combined
StarbiteUFS: host: cap 0x1303ff0f ver 0x210 hcs 0x10f hce 0x0 utrlba 0x0:ef002000 dbr 0x0 rsr 0x1 nexus 0x2
StarbiteUFS: host taken: list at 0x.., 4 slots, 32-bit DMA, polled
StarbiteUFS: ufsp: rctrl 0x.. rsecurity 0x.. region0 begin 0x0 end 0xffffffff lun 0xff ctrl 0xf1
StarbiteUFS: lun 0xd0 op 0x00 failed: ocs 0x7 ... sense 2/04/02 ...      (expected: the device is asleep)
StarbiteUFS: device W-LUN: TEST UNIT READY .., START STOP UNIT (ACTIVE) 0
StarbiteUFS: lun 0: .. blocks of 4096 bytes (.. MiB)
StarbiteUFS: device: SAMSUNG .. rev ..
StarbiteUFS: range 0 "USERDATA" lun 0 lba 1698432 +4915200 (19200 MiB) ro root uuid ..: EXPOSED
StarbiteUFS: vs_hci was tx 0x.. rx 0x.. reorder 0x.. axidma 0x.., now programmed: single-block 0/0 same
StarbiteUFS: multi-block commands on: 16 x 4 KiB entries per command (16-byte entries)
StarbiteUFS: interrupt: off (sbufsirq=0), polled every 250 us
StarbiteUFS: up: 4 slot(s), 16 block(s) per command, direct DMA, polled
StarbiteUFS: IOMedia tree (media settled):
StarbiteUFS: tree| +-o StarbiteUFS  <class StarbiteUFS>
StarbiteUFS: tree|   +-o StarbiteUFSRange  <class StarbiteUFSRange, range "USERDATA" lun 0 lba 1698432 +4915200>
StarbiteUFS: tree|     +-o IOBlockStorageDriver  <class IOBlockStorageDriver>
StarbiteUFS: tree|       +-o SAMSUNG .. Media  <class IOMedia, Size 20132659200, Whole Yes, Writable No, Content "", UUID -, BSD Name disk0>
StarbiteUFS: tree|         +-o IOMediaBSDClient  <class IOMediaBSDClient>
StarbiteUFS: fence: N requests checked, 0 refused (0 writes), M blocks allowed; doorbell audit refused 0; foreign LBA touched 0
StarbiteUFS: host: .. commands, .. errors, 0 timeouts, 0 fatal, 0 UIC errors, ..
STARBITE: sbstop=ufs: stage reached, resetting into TWRP
```
(With an inner GPT in the range, `IOGUIDPartitionScheme` and partition IOMedia with `UUID` appear under the whole
media. The iOS 27 root is HFSX directly in USERDATA: no partitions.)

**M4a pass:** the tree shows exactly one StarbiteUFSRange with a whole IOMedia of 20132659200 bytes,
`Writable No`, and the fence line says `refused 0 ... doorbell audit refused 0; foreign LBA touched 0`.
Allowed fail modes, all still ending in TWRP: `host not ready` / `did not take our request list` (nothing was
rung), `multi-block self test failed` (falls back to one block per command, still a pass), a timeout.

## M4b: root through IOMedia, interrupt on

Boot-args: M4a's without `sbufsirq=0` and `sbstop=ufs`, plus the root (read-only: no `rootrw`):
```
... rd=disk0 sbufsreport=40 sbstop=ufsroot
```
(`rd=disk0` = IOMedia matching by BSD name while StarbiteUFS is the only disk; once INTEGRATION.md's
range-UUID matching is in, `rd=sbrange:<uuid>`.) Expected, after M4a's lines (`interrupt: the nub's interrupt 0,
20 ms safety poll`):
```
BSD root: disk0, major .., minor ..
hfs: mounted <volume> on device disk0
StarbiteUFS: root = IOMedia <uuid>, fence log shows no foreign LBA touched
StarbiteUFS: fence: ... doorbell audit refused 0; foreign LBA touched 0
StarbiteUFS: host: ... completions <irq> irq / <poll> poll ...
... launchd ...
StarbiteUFS: report at sbufsreport=40 s
StarbiteUFS: tree| ...                                   (as M4a, BSD Name disk0)
StarbiteUFS: root = IOMedia <uuid>, fence log shows no foreign LBA touched
STARBITE: sbstop=ufsroot: stage reached, resetting into TWRP
```
The root line is printed the moment the root is mounted (the 1 s root watch) and again in the report, so a
userland crash after the mount does not hide it. `<uuid>` is the IOMedia's own UUID if it has one (a GPT
partition), else the range's UUID from the allow-list (USERDATA's partition GUID).

**M4b pass line:** `StarbiteUFS: root = IOMedia <uuid>, fence log shows no foreign LBA touched`
(together with `foreign LBA touched 0` and `doorbell audit refused 0` in the fence line).
Most interrupt completions should be `irq`; mostly `poll` means the interrupt is not reaching us (a finding for
StarbiteGICv2, not a failure: the safety poll carries the I/O).

## After the run (host)

Pull pmsg from TWRP (`adb shell cat /sys/fs/pstore/pmsg-ramoops-0`), grep `StarbiteUFS:` and `STARBITE`, and the
loader's crumb page report on the next normal start. Nothing on the phone needs restoring: nothing was written.

## If it goes wrong

| Seen | Meaning | Next |
|---|---|---|
| `no starbite,allowed-ranges on the nub` | the ADT lacks the properties | board file / loader (INTEGRATION.md) |
| `fence properties refused (...)` | malformed blob: nothing exposed | re-pack with sbufs-fence.py |
| `range 0 ... meets a denied extent` / `GPT head/tail floor` | the allow-list overlaps Android | fix the allow-list, never the deny-list |
| `herolte_ufs owns the host` | `rd=ufsroot0` or `ufsprobe` on the line | remove them |
| `lun 0: not ready` after the wake | device stayed asleep | compare with herolte's `ufsprobe=1` output |
| `timed out` / `FATAL host error` | the host stopped answering | `sbufsqd=1 sbufsmulti=0 sbufsirq=0` (herolte's exact mode) |
| `hfs: ... ` mount error with 4 KiB blocks | NOTES.md open question 2 | 512-byte presentation |
