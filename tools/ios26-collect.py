#!/usr/bin/env python3
"""iOS 26 card root: health check + log/crash collection from TWRP over adb, read only.
    tools/ios26-collect.py OUTDIR            health + listings + logs + newest crash reports
    tools/ios26-collect.py --health          only the volume header / journal state (a few seconds)
Reads the HFSX root at card sector 182888448 (tools/overlay/hfs-read.py with HFS_CARD=1). Nothing is
written to the phone. Health: signature, attributes (unmounted / inconsistent / journaled), last mounted
version, free space, and whether the journal holds transactions not yet replayed (start != end)."""
import importlib.util
import os
import struct
import sys
from pathlib import Path

os.environ['HFS_CARD'] = '1'
_spec = importlib.util.spec_from_file_location('hfsread', Path(__file__).with_name('overlay') / 'hfs-read.py')
hr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hr)

LIST = ['/', '/private/var/tmp', '/private/var/local', '/private/var/db', '/private/var/mobile/Library',
        '/private/var/mobile/Library/Logs', '/private/var/mobile/Library/Logs/CrashReporter',
        '/private/var/mobile/Library/PosterBoard', '/private/var/mobile/Library/SpringBoard',
        '/Library/Wallpaper', '/private/var/mobile/Containers/Data/Application']
PULL_DIRS = ['/private/var/tmp', '/private/var/local']
CRASH_DIR = '/private/var/mobile/Library/Logs/CrashReporter'
MAX_FILE = 8 << 20


def health():
    vh = hr.dev_read(1024, 512)
    sig = vh[:2]
    attrs, = struct.unpack_from('>I', vh, 4)
    lmv = vh[8:12]
    jib, = struct.unpack_from('>I', vh, 12)
    bsize, total, free = struct.unpack_from('>III', vh, 40)
    print(f'signature {sig!r}, last mounted by {lmv!r}, block {bsize}, {total * bsize >> 20} MiB, '
          f'free {free * bsize >> 20} MiB')
    print(f'attributes 0x{attrs:08x}: unmounted-cleanly={bool(attrs & 0x100)} inconsistent={bool(attrs & 0x800)} '
          f'journaled={bool(attrs & 0x2000)}')
    ok = sig in (b'H+', b'HX') and not attrs & 0x800
    if attrs & 0x2000 and jib:
        info = hr.dev_read(jib * bsize, 52)
        flags, = struct.unpack_from('>I', info, 0)
        joff, jsize = struct.unpack_from('>QQ', info, 36)
        hdr = hr.dev_read(joff, 64)
        magic_le, = struct.unpack_from('<I', hdr, 0)
        fmt = '<' if magic_le == 0x4a4e4c78 else '>'
        magic, endian, start, end, size = struct.unpack_from(fmt + 'IIQQQ', hdr, 0)
        pending = start != end
        print(f'journal: info flags 0x{flags:x}, at byte {joff} size {jsize >> 20} MiB, header magic 0x{magic:08x} '
              f'start {start} end {end}: {"TRANSACTIONS PENDING (replayed at the next mount)" if pending else "empty (clean)"}')
        ok = ok and magic == 0x4a4e4c78
    print('health:', 'OK' if ok else 'PROBLEM')
    return ok


def main():
    if len(sys.argv) == 2 and sys.argv[1] == '--health':
        sys.exit(0 if health() else 1)
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    health()
    vol = hr.Volume()
    with open(out / 'listings.txt', 'w') as f:
        for d in LIST:
            try:
                ents = vol.listdir(d)
            except SystemExit as e:
                f.write(f'== {d}: {e}\n')
                continue
            f.write(f'== {d} ({len(ents)})\n')
            for name, kind, size in ents:
                f.write(f'{kind:4} {size:>12} {name}\n')
    print('listings ->', out / 'listings.txt')
    pulls = []
    for d in PULL_DIRS:
        try:
            pulls += [(f'{d}/{n}', s) for n, k, s in vol.listdir(d) if k == 'file' and 0 < s <= MAX_FILE]
        except SystemExit:
            pass
    try:
        crashes = [(f'{CRASH_DIR}/{n}', s) for n, k, s in vol.listdir(CRASH_DIR) if k == 'file' and s <= MAX_FILE]
        pulls += sorted(crashes)[-40:]
    except SystemExit:
        pass
    pulls += [('/System/Library/xpc/launchd.plist', 0), ('/System/Library/xpc/launchd-disabled.plist', 0)]
    for path, size in pulls:
        dest = out / path.lstrip('/').replace('/', '__')
        try:
            dest.write_bytes(vol.read_path(path))
            print(f'{path}: {size} bytes')
        except SystemExit as e:
            print(f'{path}: {e}')


if __name__ == '__main__':
    main()
