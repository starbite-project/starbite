#!/usr/bin/env python3
"""Decode an SBFW table (docs/STARBITE-FIRMWARE-LAYER.md 10.2): the 4 KiB page the loader writes at
0x91FFE000 before every OS start. From the phone without an OS: the loader's fastboot builds one as the
next hand-off would:
    fastboot oem sbfw-table && fastboot get_staged t.bin && python3 tools/menu/sbfw-table.py t.bin
"""
import struct
import sys
import zlib

WHY = {0: 'menu pick', 1: 'default / auto', 2: 'boot-next', 3: 'shell mode', 4: 'retry after a crash', 5: 'fastboot'}
REQ = {0: 'none', 1: 'verbose', 2: 'safe', 3: 'single', 4: 'prev', 5: 'xnu-single', 6: 'winre', 7: 'driver-off',
       8: 'no-shell'}


def decode(p):
    if len(p) < 4096 or p[:8] != b'SBFWTBL1':
        sys.exit('not an SBFW table (magic)')
    ver, crc = struct.unpack_from('<II', p, 8)
    ok = zlib.crc32(p[16:4096]) == crc
    u = lambda o: struct.unpack_from('<I', p, o)[0]
    q = lambda o: struct.unpack_from('<Q', p, o)[0]
    s = lambda o, n=24: p[o:o + n].split(b'\0')[0].decode(errors='replace')
    medium = u(80)
    print(f'SBFW table v{ver}, CRC {"ok" if ok else "BAD"}')
    print(f'  start: {s(24) or "-"} ({WHY.get(u(76), u(76))}), one-shot {REQ.get(u(20), u(20))}, debug level {u(16)}')
    print(f'  console rows {u(48)}..{u(52)}, framebuffer {q(56):#x} stride {u(64)} fmt {u(68)}, loader {u(72)}')
    print(f'  home store: {"none" if medium == 0xffffffff else ("card" if medium == 1 else "UFS")}'
          f'{" (HOME)" if u(84) & 1 else ""}, base block {q(88)}, capacity {q(96)} blocks')
    print(f'  last start: {s(112) or "-"} kind {u(104)} severity {u(108)}; POST {u(136)} failed {u(140)} warned; '
          f'settings status {u(144)}; flags {"card-only " if u(148) & 1 else ""}{"shell-mode" if u(148) & 2 else ""}')
    for i in range(24):
        line = p[1024 + 120 * i:1024 + 120 * (i + 1)].split(b'\0')[0]
        if line:
            print('  | ' + line.decode(errors='replace'))
    return ok


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(0 if decode(open(sys.argv[1], 'rb').read()) else 1)
