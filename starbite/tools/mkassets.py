#!/usr/bin/env python3
"""Pack libsb's embedded asset container from menu v7's (docs/ENGINEER-OS.md section 5).

    mkassets.py MENU7.sb7 OUT.sb7

Keeps every FONT section and the IMAGE sections with ids below 128 (menu v7's line icons and the
provider glyphs) of build/brand/menu7.sb7 (tools/brand/menu7-assets.py), byte for byte, in a new
SB7ASSET container (shim/starlte/sb7.h: 32-byte header, 16-byte directory entries sorted by
(type, id), 16-byte aligned sections, CRC-32 of the directory and of the data). The image embeds it
(libsb/assets.S), so Engineer OS never depends on the BOOT ramdisk the loader leaves at 0x89000000.
Standard library only (runs with the VM's python3).
"""
import struct
import sys
import zlib

T_FONT, T_IMAGE = 1, 2


def main():
    src, out = sys.argv[1], sys.argv[2]
    b = open(src, 'rb').read()
    magic, ver, count, size, dcrc, xcrc, flags = struct.unpack_from('<8s6I', b, 0)
    assert magic == b'SB7ASSET' and ver == 1, f'{src}: not an SB7ASSET v1 container'
    assert size <= len(b), f'{src}: truncated'
    dend = 32 + 16 * count
    assert zlib.crc32(b[32:dend]) == dcrc and zlib.crc32(b[dend:size]) == xcrc, f'{src}: CRC mismatch'
    keep = []
    for i in range(count):
        t, sid, off, sz = struct.unpack_from('<4I', b, 32 + 16 * i)
        if t == T_FONT or (t == T_IMAGE and sid < 128):
            keep.append((t, sid, b[off:off + sz]))
    keep.sort(key=lambda e: (e[0], e[1]))
    n = len(keep)
    pos = 32 + 16 * n
    pos = (pos + 15) & ~15
    dirs, data = [], bytearray()
    base = pos
    for t, sid, blob in keep:
        off = base + len(data)
        dirs.append(struct.pack('<4I', t, sid, off, len(blob)))
        data += blob
        data += bytes((-len(data)) % 16)
    d = b''.join(dirs)
    body = bytes(base - 32 - len(d)) + bytes(data)
    total = 32 + len(d) + len(body)
    head = struct.pack('<8s6I', b'SB7ASSET', 1, n, total, zlib.crc32(d), zlib.crc32(body), 0)
    blob = head + d + body
    assert len(blob) == total
    open(out, 'wb').write(blob)
    fonts = sum(1 for e in keep if e[0] == T_FONT)
    print(f'{out}: {n} sections ({fonts} fonts, {n - fonts} images), {total} bytes')


if __name__ == '__main__':
    main()
