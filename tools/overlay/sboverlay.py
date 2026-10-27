#!/usr/bin/env python3
"""Pack the Starbite iOS overlay (store entry "ios.overlay"), installed into
the iOS root by the kernel before launchd starts (xnu/src/starlte_overlay.c).
    sboverlay.py OUT.bin LOCAL=/ios/path[:MODE] ...
    sboverlay.py build/ios/overlay.bin out/ios/sbgpu-test=/usr/bin/sbgpu-test:755
Layout: "SBOVL001", u32 count, u32 version 1, then count x 256 bytes:
char path[232], u32 mode, u32 size, u32 CRC32, u32 offset (4 KiB aligned,
from the overlay start), 8 zero bytes; data after the header, 4 KiB aligned.
Target directories must already exist on the iOS root.
"""
import struct
import sys
import zlib
from pathlib import Path

BLOCK = 4096


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    files = []
    for arg in sys.argv[2:]:
        local, _, target = arg.partition('=')
        path, _, mode = target.partition(':')
        if not path.startswith('/') or len(path.encode()) >= 232:
            sys.exit(f'bad target path {path!r}')
        files.append((path, int(mode or '644', 8), Path(local).read_bytes()))
    head = 16 + 256 * len(files)
    off = (head + BLOCK - 1) // BLOCK * BLOCK
    entries, data = bytearray(), bytearray()
    for path, mode, blob in files:
        entries += struct.pack('<232sIIII8x', path.encode(), mode, len(blob), zlib.crc32(blob), off)
        pad = (-len(blob)) % BLOCK
        data += blob + bytes(pad)
        off += len(blob) + pad
    out = struct.pack('<8sII', b'SBOVL001', len(files), 1) + bytes(4) + entries
    out += bytes((-len(out)) % BLOCK) + data
    Path(sys.argv[1]).parent.mkdir(parents=True, exist_ok=True)
    Path(sys.argv[1]).write_bytes(out)
    for path, mode, blob in files:
        print(f'{path}  {len(blob)} bytes  mode {mode:o}  crc {zlib.crc32(blob):08x}')
    print(f'{sys.argv[1]}: {len(out)} bytes, crc {zlib.crc32(out):08x}')


if __name__ == '__main__':
    main()
