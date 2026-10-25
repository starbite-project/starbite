#!/usr/bin/env python3
"""Run-length encode an overlay blob for the BOOT ramdisk slot:
"SBOVRLAY" (u32 n, then per rectangle u32 x, y, w, h + w*h ARGB8888) ->
"SBOVRRLE" (u32 n, then per rectangle u32 x, y, w, h, u32 run count, then
runs of u32 length + u32 ARGB pixel, row-major over the rectangle).
shim/starlte/xnu_boot.c draws both.
  tools/brand/rle.py build/brand/overlay.bin build/brand/overlay.rle"""
import struct, sys

src, dst = sys.argv[1], sys.argv[2]
b = open(src, "rb").read()
assert b[:8] == b"SBOVRLAY", "not an SBOVRLAY overlay"
n, = struct.unpack_from("<I", b, 8)
out = bytearray(b"SBOVRRLE" + struct.pack("<I", n))
o = 12
for _ in range(n):
    x, y, w, h = struct.unpack_from("<4I", b, o)
    o += 16
    px = struct.unpack_from(f"<{w * h}I", b, o)
    o += 4 * w * h
    runs = []
    for p in px:
        if runs and runs[-1][1] == p:
            runs[-1][0] += 1
        else:
            runs.append([1, p])
    out += struct.pack("<5I", x, y, w, h, len(runs))
    for length, p in runs:
        out += struct.pack("<2I", length, p)
open(dst, "wb").write(out)
print(f"{dst}: {n} rectangles, {len(b)} -> {len(out)} bytes")
