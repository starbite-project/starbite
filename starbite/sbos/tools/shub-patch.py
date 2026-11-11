#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""The BCM4775 sensor hub's firmware patch as a binary, from a Samsung kernel tree (docs/SENSOR-HUB.md 2.2).

  shub-patch.py KERNEL_TREE_OR_HEADER OUT.bin

Reads drivers/sensorhub/brcm/bbdpl/q_os/bbd_patch_file_star.h (a C string list: <Key=Value> lines, then <Blob=hex>),
writes the 413864-byte blob and refuses anything whose sha256 is not the known one. The blob is Broadcom / Samsung
firmware whose license is unclear: it is never committed to this repository. Keep OUT.bin outside the checkout
(starbite/sbos/build.sh embeds it into the test image starbite-os-shub.img only, from SHUB_PATCH or
~/sbos-shub-blob/bcm4775-star-q.bin, and skips that image when it is missing).
"""
import hashlib
import os
import re
import struct
import sys

REL = "drivers/sensorhub/brcm/bbdpl/q_os/bbd_patch_file_star.h"
LENGTH = 413864
SHA256 = "b1a3e138ab808f2be247ad195d71d6aef2f2d3ecc7bd3e13e141b1443da408d0"
WANT = {"AsicVersion": "0x004775B0", "Type": "Application", "PatchAddress": "0x00800161", "BlobLength": str(LENGTH)}


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    if os.path.isdir(src):
        src = os.path.join(src, REL)
    text = open(src, encoding="ascii").read()
    flat = text.replace("\\n", "").replace('"', "").replace("\n", "").replace("\r", "").replace(" ", "").replace("\t", "")
    hdr = {m.group(1): m.group(2) for m in re.finditer(r"<(\w+)=([^>]{0,40})>", flat)}
    for k, v in WANT.items():
        if hdr.get(k) != v:
            sys.exit(f"shub-patch: {src}: header {k} is {hdr.get(k)!r}, expected {v!r}")
    m = re.search(r"<Blob=([0-9A-Fa-f]+)>", flat)
    if not m:
        sys.exit(f"shub-patch: {src}: no <Blob=...>")
    blob = bytes.fromhex(m.group(1))
    digest = hashlib.sha256(blob).hexdigest()
    if len(blob) != LENGTH or digest != SHA256:
        sys.exit(f"shub-patch: {src}: {len(blob)} bytes, sha256 {digest}: not the known patch ({LENGTH}, {SHA256})")
    # the records: all 0xF1 writes, contiguous from 0x00800000
    i, n, addr = 0, 0, 0x00800000
    while i < len(blob):
        ty, ln = blob[i], struct.unpack_from("<H", blob, i + 1)[0]
        a = struct.unpack_from("<I", blob, i + 3)[0]
        if ty != 0xF1 or ln < 4 or a != addr or i + 3 + ln > len(blob):
            sys.exit(f"shub-patch: record {n} at {i}: type {ty:#x}, length {ln}, address {a:#x} (expected {addr:#x})")
        addr += ln - 4
        i += 3 + ln
        n += 1
    out = os.path.abspath(out)
    top = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    if os.path.commonpath([out, top]) == top:
        sys.exit(f"shub-patch: {out} is inside the checkout: keep the blob outside it (it is never committed)")
    with open(out, "wb") as f:
        f.write(blob)
    print(f"shub-patch: {out}: {len(blob)} bytes, sha256 ok, {n} write records, {addr - 0x00800000} bytes to {addr:#010x}")


if __name__ == "__main__":
    main()
