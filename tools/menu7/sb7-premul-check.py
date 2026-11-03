#!/usr/bin/env python3
"""Check every ARGB image of an SB7ASSET container (sb7.h): premultiplied (each of R, G, B <= A),
alpha 0 means 0x00000000, and counts opaque black 4 x 4 blocks (A = 255, RGB = 0; Tux's body is
the only expected one). This checks the assets only: the menu v7 build before the real logos had
1 to 5 colour-above-alpha pixels in each of 5 tile layers, too few to show, so the black squares
and side glitches seen on the phone are not explained by the assets (still open, in the glass and
runtime path; they need a capture on the device).

    build/venv/bin/python -I tools/menu7/sb7-premul-check.py build/brand/menu7.sb7
"""
import struct
import sys

import numpy as np


def unrle(data, count, bpp):
    out = bytearray()
    p = 0
    while len(out) < count * bpp and p + 2 <= len(data):
        c = data[p] | data[p + 1] << 8
        p += 2
        if c < 0x8000:
            out += data[p:p + c * bpp]
            p += c * bpp
        else:
            out += data[p:p + bpp] * (c - 0x8000)
            p += bpp
        p += p & 1
    return bytes(out)


def main():
    b = open(sys.argv[1], 'rb').read()
    assert b[:8] == b'SB7ASSET'
    n = struct.unpack_from('<I', b, 12)[0]
    bad = 0
    checked = 0
    for i in range(n):
        t, iid, off, size = struct.unpack_from('<IIII', b, 32 + 16 * i)
        if t != 2:
            continue
        w, h, fmt, enc, _, nx, ny, nw, nh, ds = struct.unpack_from('<HHBBHHHHHI', b, off)
        if fmt != 2:
            continue
        raw = b[off + 20:off + 20 + ds]
        px = unrle(raw, w * h, 4) if enc == 1 else raw
        a = np.frombuffer(px, '<u4').reshape(h, w)
        A, R, G, B = (a >> 24) & 255, (a >> 16) & 255, (a >> 8) & 255, a & 255
        e1 = int(((R > A) | (G > A) | (B > A)).sum())
        e2 = int(((A == 0) & (a != 0)).sum())
        blk = (A == 255) & (R + G + B < 12)
        k = blk[:-3, :-3] & blk[1:-2, 1:-2] & blk[3:, 3:] & blk[:-3, 3:] & blk[3:, :-3] if h > 4 and w > 4 else np.zeros(1, bool)
        e3 = int(k.sum())
        checked += 1
        if e1 or e2 or e3:
            print(f'image {iid}: {w}x{h} not premultiplied {e1}, alpha-0 colour {e2}, opaque black 4x4 {e3}')
            bad += (e1 + e2) > 0
    print(f'{checked} ARGB images checked, {bad} with premultiplication errors')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
