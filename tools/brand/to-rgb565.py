#!/usr/bin/env python3
"""PNG splash -> loader blob: "SBSPLASH" u32 width, u32 height, then RGB565
pixels, little endian, row-major (shim/starlte/xnu_boot.c draws it).
Decodes through macOS sips (PNG -> 32-bit BMP), no Python imaging library.
  tools/brand/to-rgb565.py build/brand/splash.png build/brand/splash.bin"""
import struct, subprocess, sys, tempfile, os

src, dst = sys.argv[1], sys.argv[2]
with tempfile.TemporaryDirectory() as tmp:
    bmp = os.path.join(tmp, "s.bmp")
    subprocess.run(["sips", "-s", "format", "bmp", src, "--out", bmp], check=True, capture_output=True)
    b = open(bmp, "rb").read()
off = struct.unpack_from("<I", b, 10)[0]
w, h = struct.unpack_from("<ii", b, 18)
bpp = struct.unpack_from("<H", b, 28)[0]
assert bpp in (24, 32), bpp
bottom_up = h > 0
h = abs(h)
stride = (w * bpp // 8 + 3) & ~3
out = bytearray(b"SBSPLASH" + struct.pack("<II", w, h))
for y in range(h):
    row = off + (h - 1 - y if bottom_up else y) * stride
    for x in range(w):
        p = row + x * (bpp // 8)
        bl, g, r = b[p], b[p + 1], b[p + 2]
        out += struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3))
open(dst, "wb").write(out)
print(f"{dst}: {w}x{h}, {len(out)} bytes")
