#!/usr/bin/env python3
"""Print the file version (VS_FIXEDFILEINFO) and PE machine of Windows binaries.

  pever.py FILE [FILE ...]

Used to check that a WinPE/WinRE image carries the winload.efi / ntoskrnl.exe build the
woa firmware is matched to (22621.7582 / 22631.7584). Reads only; no dependencies.
"""
import struct
import sys

MACHINES = {0xAA64: 'arm64', 0x8664: 'x64', 0x14C: 'x86', 0x1C4: 'arm'}


def version(data):
    i = data.find(struct.pack('<I', 0xFEEF04BD))
    while i >= 0:
        sig, _, ms, ls = struct.unpack_from('<IIII', data, i)
        if ms >> 16 in (6, 10):          # Windows 10/11 binaries say 10.0
            return f'{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}'
        i = data.find(struct.pack('<I', 0xFEEF04BD), i + 4)
    return '?'


def machine(data):
    if data[:2] != b'MZ':
        return 'not PE'
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    if data[pe:pe + 4] != b'PE\0\0':
        return 'not PE'
    return MACHINES.get(struct.unpack_from('<H', data, pe + 4)[0], 'other')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for path in sys.argv[1:]:
        try:
            data = open(path, 'rb').read()
        except OSError as e:
            print(f'{path}  ({e.strerror})')
            continue
        if not data:
            print(f'{path}  (missing)')
            continue
        print(f'{path}  {version(data)}  {machine(data)}  {len(data)} bytes')


if __name__ == '__main__':
    main()
