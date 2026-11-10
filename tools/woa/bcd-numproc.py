#!/usr/bin/env python3
"""Show or change `numproc` (BCD element 25000061) of every Windows loader in a BCD hive, in place.

  bcd-numproc.py BCD                 show each object's 25000061 value and where its 8 bytes are
  bcd-numproc.py BCD OUT N           write a copy with every existing 25000061 set to N (1..64)

The BCD is a registry hive; Objects\\{id}\\Elements\\25000061 holds an "Element" value of type
REG_BINARY, 8 bytes, little endian (docs/WIN11-24H2-A55.md; the same edit as NOTES 2026-10-26's
numproc 4 -> 1 at file 0x409c). Only those 8 data bytes change: no cell is moved or resized, so the
hive stays valid as long as it was clean (both sequence numbers equal) before; a dirty hive is
refused. Nothing is added or removed: a loader without numproc stays without it.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util
_spec = importlib.util.spec_from_file_location('hive', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hive.py'))
hv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hv)


def locate(h):
    """[(object id, file offset of the 8 data bytes, current value)] for every Element of 25000061."""
    out = []
    _, objs = h.find('Objects')
    for _, ok in h.subkeys(objs):
        for _, ek in h.subkeys(ok):
            if ek['name'].lower() != 'elements':
                continue
            for _, el in h.subkeys(ek):
                if el['name'] != '25000061':
                    continue
                lo, _ = h.cell(el['vlist'])
                for i in range(el['nval']):
                    vo, _ = h.cell(struct.unpack_from('<I', h.d, lo + 4 * i)[0])
                    nlen, dlen, doff, typ, flags = struct.unpack_from('<HIIIH', h.d, vo + 2)
                    name = h.d[vo + 0x14:vo + 0x14 + nlen].decode('latin-1' if flags & 1 else 'utf-16-le')
                    if name != 'Element':
                        continue
                    if typ != 3 or dlen != 8:
                        raise SystemExit(f'{ok["name"]}: 25000061 Element type {typ} length {dlen:#x}, expected REG_BINARY 8')
                    co, _ = h.cell(doff)
                    out.append((ok['name'], co, struct.unpack_from('<Q', h.d, co)[0]))
    return out


def main():
    a = sys.argv[1:]
    if len(a) not in (1, 3):
        sys.exit(__doc__)
    h = hv.Hive(a[0])
    s1, s2 = struct.unpack_from('<II', h.d, 4)
    found = locate(h)
    for oid, off, v in found:
        print(f'{oid}: numproc {v} (8 bytes at file {off:#x})')
    if not found:
        print('no object has numproc (25000061)')
    if len(a) == 1:
        print(f'hive sequence {s1}/{s2} ({"clean" if s1 == s2 else "DIRTY"})')
        return
    n = int(a[2], 0)
    if not 1 <= n <= 64:
        sys.exit('N must be 1..64')
    if s1 != s2:
        sys.exit(f'hive is dirty (sequence {s1}/{s2}): not editing; boot Windows once and shut it down cleanly')
    if not found:
        sys.exit('nothing to change')
    d = bytearray(h.d)
    for oid, off, v in found:
        struct.pack_into('<Q', d, off, n)
    open(a[1], 'wb').write(bytes(d))
    for oid, off, v in locate(hv.Hive(a[1])):
        print(f'written: {oid}: numproc {v}')
    diff = [i for i in range(len(d)) if d[i] != h.d[i]]
    print(f'{len(diff)} byte(s) changed, at {", ".join(hex(i) for i in diff)}')


if __name__ == '__main__':
    main()
