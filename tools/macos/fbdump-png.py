#!/usr/bin/env python3
"""SWEET FBDUMP lines (kernel patch 0375, sweetfbdump=N) from a kernel log -> PNG (each 12-bit
pixel stretched to 8 bits, upscaled 2x).   fbdump-png.py kernel.log out.png
Several dumps (0385): out.png is the last, out-1.png, out-2.png ... each one."""
import re, sys, zlib, struct
W = H = None
px = {}
dumps = []
for m in re.finditer(rb'SWEET FBDUMP (\d+) (\d+) (\d+) (\d+) ([0-9a-f]+)|SWEET FBDUMP end', open(sys.argv[1], 'rb').read()):
    if m.group(1) is None:
        dumps.append(dict(px)); continue
    W, H, y, x0 = (int(g) for g in m.groups()[:4])
    h = m.group(5).decode()
    for i in range(len(h) // 3):
        px[(x0 + i, y)] = tuple(int(c, 16) * 17 for c in h[i * 3:i * 3 + 3])
if W is None:
    sys.exit('no FBDUMP lines')
if not dumps:
    dumps = [px]
S = 2
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
def png(px, path):
    raw = bytearray()
    for y in range(H * S):
        raw.append(0)
        for x in range(W * S):
            raw += bytes(px.get((x // S, y // S), (255, 0, 255)))
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W * S, H * S, 8, 2, 0, 0, 0))
                          + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))
base = sys.argv[2][:-4] if sys.argv[2].endswith('.png') else sys.argv[2]
for i, d in enumerate(dumps):
    png(d, f'{base}-{i + 1}.png')
px = dumps[-1]
raw = bytearray()
for y in range(H * S):
    raw.append(0)
    for x in range(W * S):
        raw += bytes(px.get((x // S, y // S), (255, 0, 255)))
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
open(sys.argv[2], 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W * S, H * S, 8, 2, 0, 0, 0))
                              + chunk(b'IDAT', zlib.compress(bytes(raw), 9)) + chunk(b'IEND', b''))
print(f'{W}x{H}, {len(dumps)} dump(s), last {len(px)}/{W * H} pixels -> {sys.argv[2]} (+ {base}-N.png)')
