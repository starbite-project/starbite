#!/usr/bin/env python3
"""Patch ChromiumOS R158's librootdev.so.1.0 so chromeos_startup finds its root and stateful on a loop disk.

Our init attaches the whole GPT image with `losetup -P` (no loop.max_part: ChromeOS's libbrillo loop code
opens /dev/loop<minor>, which max_part breaks), so ROOT-A is /dev/loop0p3 = 259:x under the 7:0 disk.

  1. rodata 0x10ac "loop" -> "\\x01oop": rootdev stops treating loop devices as partitionless.
  2. match_sysfs_device 0x183c/0x1840: it only descended into /sys/block/<disk> when the disk's major
     equalled the root's (cmp w25,w0 / b.ne); now it descends into any entry with a device number
     (cmp x24,#0 / b.eq), so 259:x partitions are found under loop0.

Checked in the VM: chroot into the image's ROOT-A, `rootdev` -> /dev/loop0p3, `rootdev -d` -> /dev/loop0.

Usage: patch-rootdev.py FILE [BASE]
  FILE is the library, or the whole chromeos.img with BASE = the library's byte offset in it
  (R158-16868.12.0 test image: 2011168768, the file's first data block in ROOT-A is contiguous).
"""
import sys

PATCHES = [
    (0x10ac, bytes.fromhex("6c6f6f70"), bytes.fromhex("016f6f70")),
    (0x183c, bytes.fromhex("3f03006ba1"), bytes.fromhex("1f0300f1a0")),
]

path = sys.argv[1]
base = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
with open(path, "r+b") as f:
    for off, old, new in PATCHES:
        f.seek(base + off)
        cur = f.read(len(old))
        if cur == new:
            print(f"0x{off:x}: already patched")
            continue
        if cur != old:
            sys.exit(f"0x{off:x}: unexpected bytes {cur.hex()} (want {old.hex()}): refusing")
        f.seek(base + off)
        f.write(new)
        print(f"0x{off:x}: {old.hex()} -> {new.hex()}")
