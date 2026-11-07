#!/usr/bin/env python3
"""Pack a menuchain store image: the chain stub, then the loader Image, then (optionally) a BOOT ramdisk.

    pack.py STUB.bin LOADER.Image OUT.img [--ramdisk RD]

The stub's header gets image_size (the whole file) and the chain header (offsets, lengths)."""
import argparse, hashlib, struct, sys
from pathlib import Path

ap = argparse.ArgumentParser()
ap.add_argument("stub"); ap.add_argument("loader"); ap.add_argument("out")
ap.add_argument("--ramdisk")
a = ap.parse_args()
stub = bytearray(Path(a.stub).read_bytes())
ldr = Path(a.loader).read_bytes()
rd = Path(a.ramdisk).read_bytes() if a.ramdisk else b""
assert stub[56:60] == b"ARMd" and ldr[56:60] == b"ARMd", "not arm64 Images"
assert struct.unpack_from("<Q", stub, 64)[0] == 0x314E49414843374D, "chain header missing"
pad = lambda b: b + b"\0" * (-len(b) % 4096)
stub = pad(stub)
loff = len(stub)
lbody = pad(ldr)
roff = loff + len(lbody) if rd else 0
img = stub + lbody + (pad(rd) if rd else b"")
img = bytearray(img)
struct.pack_into("<Q", img, 16, len(img))                  # image_size
struct.pack_into("<QQQQQ", img, 72, loff, (len(ldr) + 7) & ~7, roff, len(rd), 0x80080000)
Path(a.out).write_bytes(img)
print(f"{a.out}: {len(img)} bytes, loader {len(ldr)} at {loff:#x}, ramdisk {len(rd)} at {roff:#x}, "
      f"sha256 {hashlib.sha256(img).hexdigest()}")
