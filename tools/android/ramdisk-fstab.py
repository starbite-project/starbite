#!/usr/bin/env python3
"""Swap files inside an Android first-stage ramdisk (gzip or plain newc cpio), byte-exact elsewhere.

  ramdisk-fstab.py list RAMDISK
  ramdisk-fstab.py cat RAMDISK NAME
  ramdisk-fstab.py put RAMDISK OUT NAME=FILE [NAME=FILE ...]

Used for docs/ANDROID-PLAN.md: a GSI or card-booted Android gets the same first-stage init as its
vendor's boot.img, with only fstab.samsungexynos9810 replaced (system/vendor/odm/metadata on the
microSD). Every other member keeps its mode, uid/gid, mtime and data; replaced members keep their
header fields except the size (and the input's hex case). The output is gzip -9 when the input
was gzip (the 4.9 kernel has CONFIG_RD_GZIP). Nothing here touches a device.
"""
import gzip
import sys
from pathlib import Path

MAGIC = b'070701'
FIELDS = ('ino', 'mode', 'uid', 'gid', 'nlink', 'mtime', 'filesize', 'devmajor', 'devminor',
          'rdevmajor', 'rdevminor', 'namesize', 'check')


def pad4(n):
    return (4 - n % 4) % 4


def parse(blob):
    out, off = [], 0
    while True:
        if blob[off:off + 6] != MAGIC:
            sys.exit(f'not a newc cpio at offset {off}')
        h = {k: int(blob[off + 6 + 8 * i: off + 14 + 8 * i], 16) for i, k in enumerate(FIELDS)}
        p = off + 110
        name = blob[p:p + h['namesize'] - 1].decode()
        p += h['namesize']
        p += pad4(p)
        data = blob[p:p + h['filesize']]
        p += h['filesize']
        p += pad4(p)
        out.append((name, h, data))
        off = p
        if name == 'TRAILER!!!':
            return out, blob[off:]


def emit(entries, tail, upper):
    b = bytearray()
    fmt = b'%08X' if upper else b'%08x'
    for name, h, data in entries:
        h = dict(h, filesize=len(data), namesize=len(name.encode()) + 1)
        b += MAGIC + b''.join(fmt % h[k] for k in FIELDS)
        b += name.encode() + b'\0'
        b += b'\0' * pad4(len(b))
        b += data
        b += b'\0' * pad4(len(b))
    return bytes(b) + tail


def load(path):
    raw = Path(path).read_bytes()
    if raw[:2] == b'\x1f\x8b':
        return gzip.decompress(raw), True
    return raw, False


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    cmd, path = argv[1], argv[2]
    blob, gz = load(path)
    entries, tail = parse(blob)
    if cmd == 'list':
        for name, h, data in entries:
            print(f'{h["mode"]:06o} {h["uid"]:>5}:{h["gid"]:<5} {len(data):>9} {name}')
    elif cmd == 'cat':
        for name, h, data in entries:
            if name == argv[3]:
                sys.stdout.buffer.write(data)
                return
        sys.exit(f'{argv[3]}: not in the ramdisk')
    elif cmd == 'put':
        out = argv[3]
        repl = dict(a.split('=', 1) for a in argv[4:])
        seen = set()
        new = []
        for name, h, data in entries:
            if name in repl:
                data = Path(repl[name]).read_bytes()
                seen.add(name)
            new.append((name, h, data))
        missing = set(repl) - seen
        if missing:
            sys.exit(f'not in the ramdisk (only existing members can be replaced): {sorted(missing)}')
        upper = any(c in b'ABCDEF' for c in blob[6:110])
        cpio = emit(new, tail, upper)
        if parse(cpio)[0] is None:
            sys.exit('re-parse failed')
        Path(out).write_bytes(gzip.compress(cpio, 9, mtime=0) if gz else cpio)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
