#!/usr/bin/env python3
"""SWEET FBDUMP lines (kernel patch 0375, sweetfbdump=N) from a kernel log -> PNG (each 12-bit
pixel stretched to 8 bits, upscaled 2x).   fbdump-png.py kernel.log out.png"""
import re, sys, zlib, struct
W = H = None
px = {}
for m in re.finditer(rb'SWEET FBDUMP (\d+) (\d+) (\d+) (\d+) ([0-9a-f]+)', open(sys.argv[1], 'rb').read()):
    W, H, y, x0 = (int(g) for g in m.groups()[:4])
    h = m.group(5).decode()
    for i in range(len(h) // 3):
        px[(x0 + i, y)] = tuple(int(c, 16) * 17 for c in h[i * 3:i * 3 + 3])
if W is None:
    sys.exit('no FBDUMP lines')
S = 2
raw = bytearray()
for y in range(H * S):
    raw.append(0)
    for x in range(W * S):
        raw += bytes(px.get((x // S, y // S), (255, 0, 255)))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W * S, H * S, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))
print(f'{W}x{H}, {len(px)}/{W * H} pixels -> {sys.argv[2]}')
