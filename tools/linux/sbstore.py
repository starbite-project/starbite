#!/usr/bin/env python3
"""Pack the Starbite OS store (read by shim/starlte/xnu_boot.c star_linux_store).

    sbstore.py OUT.img NAME=FILE [NAME=FILE ...]
    sbstore.py OUT.img linux.image=build/linux/Image linux.dtb=build/linux/exynos9810-starlte.dtb

The store lives in USERDATA at +16 GiB (between the iOS root and the Kiosk
root, 1 GiB = 262144 blocks of 4 KiB). Block 0:
  "SBSTORE1", u32 version 1, u32 count, u64 size in 4 KiB blocks, zeros to 64;
  from +64, count x 64-byte entries: char name[24] (NUL padded), u64 offset
  (bytes from the store start, 4 KiB aligned), u64 size, u32 CRC32 (IEEE,
  zlib.crc32), u32 flags (0), 8 bytes zero.
Data follows from block 1, each entry 4 KiB aligned.
"""
import struct
import sys
import zlib
from pathlib import Path

BLOCK = 4096
MAX_BLOCKS = 262144


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    entries, data = [], bytearray()
    off = BLOCK
    for arg in sys.argv[2:]:
        name, _, path = arg.partition('=')
        blob = Path(path).read_bytes()
        if not name or len(name) > 23:
            sys.exit(f'bad entry name {name!r}')
        entries.append((name, off, len(blob), zlib.crc32(blob)))
        pad = (-len(blob)) % BLOCK
        data += blob + bytes(pad)
        off += len(blob) + pad
    if len(entries) > 63:
        sys.exit('too many entries')
    blocks = off // BLOCK
    if blocks > MAX_BLOCKS:
        sys.exit(f'store too large: {blocks} blocks > {MAX_BLOCKS}')
    hdr = bytearray(BLOCK)
    struct.pack_into('<8sIIQ', hdr, 0, b'SBSTORE1', 1, len(entries), blocks)
    for i, (name, eoff, size, crc) in enumerate(entries):
        struct.pack_into('<24sQQII8x', hdr, 64 + 64 * i, name.encode(), eoff, size, crc, 0)
    out.write_bytes(bytes(hdr) + bytes(data))
    for name, eoff, size, crc in entries:
        print(f'  {name:<24} offset {eoff:#010x} size {size:>10} crc32 {crc:08x}')
    print(f'{out}: {blocks} blocks ({blocks * BLOCK / 2**20:.1f} MiB) of {MAX_BLOCKS}')


if __name__ == '__main__':
    main()
