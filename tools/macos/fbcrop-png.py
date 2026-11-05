#!/usr/bin/env python3
"""SWEET FBCROP lines (kernel 0391, sweetfbcrop=X,Y,W,H) -> PNGs at full resolution, one per dump.
   fbcrop-png.py kernel.log out.png   -> out-1.png, out-2.png, ..."""
import re, struct, sys, zlib
W = H = None
px, crops = {}, []
for m in re.finditer(rb'SWEET FBCROP (\d+) (\d+) (\d+) (\d+) ([0-9a-f]+)|SWEET FBCROP end', open(sys.argv[1], 'rb').read()):
    if m.group(1) is None:
        crops.append(dict(px)); px = {}; continue
    W, H, y, x0 = (int(g) for g in m.groups()[:4])
    h = m.group(5).decode()
    for i in range(len(h) // 3):
        px[(x0 + i, y)] = tuple(int(c, 16) * 17 for c in h[i * 3:i * 3 + 3])
if W is None:
    sys.exit('no FBCROP lines')
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
base = sys.argv[2][:-4] if sys.argv[2].endswith('.png') else sys.argv[2]
for n, c in enumerate(crops or [px]):
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            raw += bytes(c.get((x, y), (255, 0, 255)))
    open(f'{base}-{n + 1}.png', 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
                                          + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))
print(f'{W}x{H}, {len(crops)} crop(s) -> {base}-N.png')
