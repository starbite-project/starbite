#!/usr/bin/env python3
"""mkfont.py IN.psf[.gz] OUT.psf [--scale N]: a PSF2 console font, each glyph scaled N times (default 2),
for Starbite Maintenance's status screen (fbcon's built-in 8x16 is about 0.4 mm wide on the S9's
1440x2960 panel). IN is PSF1 or PSF2; the Unicode table is kept. Used by tools/maint/env/mk-maint.sh
with the build VM's Terminus 32x16 (console-setup): 16x32 as is, and 32x64 scaled."""
import gzip
import struct
import sys

PSF2_MAGIC = b'\x72\xb5\x4a\x86'


def load(path):
    d = open(path, 'rb').read()
    if d[:2] == b'\x1f\x8b':
        d = gzip.decompress(d)
    if d[:4] == PSF2_MAGIC:
        _, _, hsz, flags, n, csz, h, w = struct.unpack_from('<4sIIIIIII', d, 0)
        glyphs = d[hsz:hsz + n * csz]
        table = d[hsz + n * csz:] if flags & 1 else b''
        return w, h, n, glyphs, table
    if d[:2] == b'\x36\x04':
        mode, h = d[2], d[3]
        n = 512 if mode & 1 else 256
        glyphs = d[4:4 + n * h]
        table = b''
        if mode & 2:                                  # UCS-2 table -> PSF2's UTF-8 table
            t, p, out = d[4 + n * h:], 0, bytearray()
            for _ in range(n):
                while p + 1 < len(t):
                    u, = struct.unpack_from('<H', t, p)
                    p += 2
                    if u == 0xFFFF:
                        out.append(0xFF)
                        break
                    if u == 0xFFFE:
                        out.append(0xFE)
                    else:
                        out += chr(u).encode('utf-8')
            table = bytes(out)
        return 8, h, n, glyphs, table
    sys.exit(f'{path}: not a PSF1/PSF2 font')


def scale(w, h, n, glyphs, k):
    rb, orb = (w + 7) // 8, (w * k + 7) // 8
    out = bytearray()
    for g in range(n):
        for y in range(h):
            row = int.from_bytes(glyphs[(g * h + y) * rb:(g * h + y + 1) * rb], 'big')
            bits = 0
            for x in range(w):
                bit = (row >> (rb * 8 - 1 - x)) & 1
                for _ in range(k):
                    bits = (bits << 1) | bit
            bits <<= orb * 8 - w * k
            out += bits.to_bytes(orb, 'big') * k
    return w * k, h * k, bytes(out)


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    k = int(argv[argv.index('--scale') + 1]) if '--scale' in argv else 2
    w, h, n, glyphs, table = load(argv[0])
    w2, h2, g2 = (w, h, glyphs) if k == 1 else scale(w, h, n, glyphs, k)
    csz = ((w2 + 7) // 8) * h2
    hdr = struct.pack('<4sIIIIIII', PSF2_MAGIC, 0, 32, 1 if table else 0, n, csz, h2, w2)
    open(argv[1], 'wb').write(hdr + g2 + table)
    print(f'{argv[1]}: {n} glyphs {w2}x{h2}')


if __name__ == '__main__':
    main(sys.argv[1:])
