#!/usr/bin/env python3
"""Replay a hive's new-format transaction logs (HvLE entries) the way Windows 8.1+ does: entries from both logs,
in sequence order, starting at the primary's secondary sequence number, consecutive, stopping at the first gap or
malformed entry. Read-only on inputs.   hive-replay.py PRIMARY OUT [LOG...]"""
import struct, sys
prim = bytearray(open(sys.argv[1], 'rb').read())
s1, s2 = struct.unpack_from('<II', prim, 4)
ents = {}
for lp in sys.argv[3:]:
    L = open(lp, 'rb').read()
    if len(L) < 0x200 or L[:4] != b'regf':
        continue
    off = 0x200
    while off + 40 <= len(L) and L[off:off + 4] == b'HvLE':
        sz, fl, seq, hbl, npg = struct.unpack_from('<IIIII', L, off + 4)
        if sz < 40 or off + sz > len(L):
            break
        refs = [struct.unpack_from('<II', L, off + 40 + 8 * i) for i in range(npg)]
        p = off + 40 + 8 * npg
        pages = []
        for o, n in refs:
            pages.append((o, L[p:p + n])); p += n
        ents.setdefault(seq, (hbl, pages, lp))
        off += sz
want = s2
applied = []
while want in ents:
    hbl, pages, lp = ents[want]
    need = 0x1000 + hbl
    if len(prim) < need:
        prim += bytes(need - len(prim))
    for o, data in pages:
        prim[0x1000 + o:0x1000 + o + len(data)] = data
    struct.pack_into('<I', prim, 0x28, hbl)
    applied.append(want); want += 1
if applied:
    last = applied[-1]
    struct.pack_into('<II', prim, 4, last, last)
    x = 0
    for i in range(0, 0x1fc, 4): x ^= struct.unpack_from('<I', prim, i)[0]
    struct.pack_into('<I', prim, 0x1fc, x)
open(sys.argv[2], 'wb').write(prim)
print(f'{sys.argv[1]}: primary {s1}/{s2}, log seqs {sorted(ents)}, applied {applied}')
