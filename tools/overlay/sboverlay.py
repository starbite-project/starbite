#!/usr/bin/env python3
"""Pack the Starbite iOS overlay (store entry "ios.overlay"), installed into
the iOS root by the kernel before launchd starts (xnu/src/starlte_overlay.c).
    sboverlay.py OUT.bin LOCAL=/ios/path[:MODE[:UID[:GID]]] ...
    sboverlay.py build/ios/overlay.bin out/ios/sbgpu-test=/usr/bin/sbgpu-test:755
Layout: "SBOVL001", u32 count, u32 version 1, then count x 256 bytes:
char path[232], u32 mode, u32 size, u32 CRC32, u32 offset (4 KiB aligned,
from the overlay start), u32 uid, u32 gid (0 = root:wheel; kernels before 0403 ignore them); data after the header, 4 KiB aligned.
Target directories must already exist on the iOS root.
Operations (kernel 0406+, card overlay only; the file then says "SBOVL002", which older kernels ignore):
    rename:/from=/to                 move a file or folder aside (undo: rename:/to=/from)
    attr:/path:MODE:UID[:GID]        set owner and mode of an existing file or folder
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
    ops = False
    for arg in sys.argv[2:]:
        if arg.startswith('rename:'):
            src, _, dst = arg[7:].partition('=')
            if not src.startswith('/') or not dst.startswith('/') or len(src.encode()) >= 232 or len(dst.encode()) >= 232:
                sys.exit(f'bad rename {arg!r}')
            files.append((src, 0x81000000, dst.encode(), 0, 0))
            ops = True
            continue
        if arg.startswith('attr:'):
            path, _, rest = arg[5:].partition(':')
            mode, _, owner = rest.partition(':')
            uid, _, gid = owner.partition(':')
            if not path.startswith('/') or not mode or not uid:
                sys.exit(f'bad attr {arg!r}')
            files.append((path, 0x82000000 | (int(mode, 8) & 0o7777), b'', int(uid), int(gid or uid)))
            ops = True
            continue
        local, _, target = arg.partition('=')
        path, _, rest = target.partition(':')
        mode, _, owner = rest.partition(':')
        uid, _, gid = owner.partition(':')
        if not path.startswith('/') or len(path.encode()) >= 232:
            sys.exit(f'bad target path {path!r}')
        files.append((path, int(mode or '644', 8), Path(local).read_bytes(), int(uid or 0), int(gid or uid or 0)))
    head = 16 + 256 * len(files)
    off = (head + BLOCK - 1) // BLOCK * BLOCK
    entries, data = bytearray(), bytearray()
    for path, mode, blob, uid, gid in files:
        entries += struct.pack('<232sIIIIII', path.encode(), mode, len(blob), zlib.crc32(blob), off, uid, gid)
        pad = (-len(blob)) % BLOCK
        data += blob + bytes(pad)
        off += len(blob) + pad
    magic = b'SBOVL002' if ops else b'SBOVL001'
    out = struct.pack('<8sII', magic, len(files), 2 if ops else 1) + entries      # entries at byte 16 (starlte_overlay.c)
    out += bytes((-len(out)) % BLOCK) + data
    Path(sys.argv[1]).parent.mkdir(parents=True, exist_ok=True)
    Path(sys.argv[1]).write_bytes(out)
    for path, mode, blob, uid, gid in files:
        print(f'{path}  {len(blob)} bytes  mode {mode:o}  owner {uid}:{gid}  crc {zlib.crc32(blob):08x}')
    print(f'{sys.argv[1]}: {len(out)} bytes, crc {zlib.crc32(out):08x}')


if __name__ == '__main__':
    main()
