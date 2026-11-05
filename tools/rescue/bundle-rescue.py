#!/usr/bin/env python3
"""Append a Starbite Rescue bundle ("SBRESCUE") to a SWEETXNU payload that may already carry
bundles (tools/linux-bundle.py). The loader (shim/starlte/xnu_boot.c rescue_bundle) finds it
without UFS, the store or the catalog. Append it LAST: loaders older than the rescue bundle stop
walking at a magic they do not know, so a bundle after it would be lost to them.

Layout (all little endian): the bundle page (u64 "SBRESCUE", u64 size of what follows the page,
zeros to 4 KiB), then the data:
  +0   "SBRSCU01"
  +8   u32 version 1, u32 flags 0
  +16  3 x { u64 offset, u64 size, u64 CRC32 }   Image, DTB, initrd; offsets from +0, 4 KiB aligned
  +88  char cmdline[256]                          appended to the DTB's bootargs (with sb.rescue=1)
  4 KiB: the Image, the DTB, the initrd, each padded to 4 KiB.

  bundle-rescue.py IN OUT --image Image --dtb linux.dtb --initrd rescue.initrd [--cmdline "..."]
"""
import argparse
import struct
import zlib
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("payload")
ap.add_argument("out")
ap.add_argument("--image", required=True)
ap.add_argument("--dtb", required=True)
ap.add_argument("--initrd", required=True)
ap.add_argument("--cmdline", default="")
args = ap.parse_args()

img = Path(args.image).read_bytes()
dtb = Path(args.dtb).read_bytes()
rd = Path(args.initrd).read_bytes()
assert img[56:60] == b"ARMd", f"{args.image} is not an arm64 Image"
assert dtb[:4] == b"\xd0\x0d\xfe\xed", f"{args.dtb} is not a flattened device tree"
assert len(dtb) <= 2 << 20, "DTB over 2 MiB (the loader's limit)"
assert len(rd) <= 80 << 20, "initrd over 80 MiB (xnu_boot.c OS_RD .. EDIT_FDT)"
cmd = args.cmdline.encode()
assert len(cmd) < 256 and b"\0" not in cmd, "cmdline must be under 256 bytes"


def pad(b: bytes) -> bytes:
    return b + b"\0" * (-len(b) % 4096)


parts, body, off = [], b"", 4096
for blob in (img, dtb, rd):
    parts.append((off, len(blob), zlib.crc32(blob) & 0xFFFFFFFF))
    body += pad(blob)
    off += len(pad(blob))
head = b"SBRSCU01" + struct.pack("<II", 1, 0)
for o, n, c in parts:
    head += struct.pack("<QQQ", o, n, c)
assert len(head) == 88
head += cmd + b"\0" * (256 - len(cmd))
data = pad(head) + body

out = Path(args.payload).read_bytes()
out += b"\0" * (-len(out) % 4096)
page = b"SBRESCUE" + struct.pack("<Q", len(data))
out += page + b"\0" * (4096 - len(page)) + data
Path(args.out).write_bytes(out)
print(f"SBRESCUE: Image {len(img)}, DTB {len(dtb)}, initrd {len(rd)} bytes; {args.out}: {len(out)} bytes")
