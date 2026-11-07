#!/usr/bin/env python3
"""Merge SB7ASSET containers (docs/STARBITE-OS.md 3.1): libsb's subset of menu v7's assets plus Starbite
OS's own sections (sbos-assets.py). A section id present in both is an error. Standard library only (runs
with the VM's python3).

    mergeassets.py OUT IN1 IN2 ...
"""
import struct
import sys
import zlib


def read(path):
    b = open(path, 'rb').read()
    magic, ver, count, size, dcrc, xcrc, _ = struct.unpack_from('<8s6I', b, 0)
    assert magic == b'SB7ASSET' and ver == 1, f'{path}: not an SB7ASSET v1 container'
    dend = 32 + 16 * count
    assert zlib.crc32(b[32:dend]) == dcrc and zlib.crc32(b[dend:size]) == xcrc, f'{path}: CRC mismatch'
    out = {}
    for i in range(count):
        t, sid, off, sz = struct.unpack_from('<4I', b, 32 + 16 * i)
        out[(t, sid)] = b[off:off + sz]
    return out


def main():
    out, ins = sys.argv[1], sys.argv[2:]
    secs = {}
    for p in ins:
        for k, v in read(p).items():
            assert k not in secs, f'{p}: section {k} twice'
            secs[k] = v
    keys = sorted(secs)
    n = len(keys)
    base = (32 + 16 * n + 15) & ~15
    dirs, data = bytearray(), bytearray()
    for k in keys:
        dirs += struct.pack('<4I', k[0], k[1], base + len(data), len(secs[k]))
        data += secs[k]
        data += bytes((-len(data)) % 16)
    body = bytes(base - 32 - len(dirs)) + bytes(data)
    total = 32 + len(dirs) + len(body)
    head = struct.pack('<8s6I', b'SB7ASSET', 1, n, total, zlib.crc32(bytes(dirs)), zlib.crc32(body), 0)
    open(out, 'wb').write(head + bytes(dirs) + body)
    print(f'{out}: {n} sections, {total} bytes')


if __name__ == '__main__':
    main()
