#!/usr/bin/env python3
"""Build the S9's PPTT in the shape Windows-on-Arm OEM firmware uses (docs/WIN11-24H2-A55.md, "PPTT").

  pptt-gen.py OUT.aml [--valid] [--uids A55=0,1,2,3 M3=4,5,6,7]
  pptt-gen.py --decode IN.aml

Why this shape. Every shipping Qualcomm Windows device (Yoga C630/SDM850, Surface Pro X, ThinkPad
X13s, Windows Dev Kit 2023, XPS 13 9345; tables in github.com/aarch64-laptops/build misc/*/pptt.dsl)
has the same PPTT: revision 1, one physical-package node holding an ID structure, one processor
node per core (flags 0x2: ACPI ID valid, no clusters, no leaf flag), and *placeholder* cache
structures whose flags and fields are all zero, shared by every core (two level-1 caches whose
"next level" is one level-2 cache, plus a level-3 cache on the package). Windows takes the cache
geometry from the CPU (CLIDR/CCSIDR), so the table only says which caches exist.

The woa release PPTT has no cache structures at all; the build-time PPTT of 2026-11-07 (PpttBuild,
revision 3, 28-byte caches with valid geometry, clusters) gave an early 0x7E. This one keeps to the
OEM shape, with one difference the S9 needs: the two core types have different cache depths
(A55: CLIDR 0x82000023 = L1 I+D, L2; no L3. M3: CLIDR 0x0B200123 = L1 I+D, L2, L3), so each type
gets its own placeholder chain (A55: L1I, L1D -> L2; M3: L1I, L1D -> L2 -> L3) and no cache hangs
off the package (a package-level L3 would claim a level the A55s don't have).

--valid fills the A55 chain with the geometry measured on the phone (zone 0, PpttFix, 2026-11-07)
and sets the valid flags; the M3 chain stays zero (never measured). Off by default (OEM shape).

ACPI processor IDs are the MADT GICC UIDs: the release MADT has A55 0..3 = UID 0..3, M3 0..3 =
UID 4..7.
"""
import struct
import sys

OEM_ID = b'EXYNOS'
OEM_TABLE_ID = b'STAR2LTE'
OEM_REV = 1
CREATOR_ID = b'SBTE'
CREATOR_REV = 1
REVISION = 1

CACHE_LEN = 24          # revision 1/2 cache type structure
NODE_LEN = 20           # + 4 per private resource
ID_LEN = 30

# kind: 0 data, 1 instruction, 2 unified (attributes bits 3:2)
A55_GEOMETRY = {        # level, kind -> (size, sets, ways, line)
    (1, 0): (32768, 128, 4, 64),
    (1, 1): (32768, 128, 4, 64),
    (2, 2): (524288, 512, 16, 64),
}


def cache(next_off, level, kind, valid, geom=None):
    if valid and geom:
        size, sets, ways, line = geom
        alloc = 0 if kind == 1 else 3            # I: read allocate; D/U: read+write
        flags = 0x7F
        attr = alloc | (kind << 2)               # write-back
        return struct.pack('<BBHIIIIBBH', 1, CACHE_LEN, 0, flags, next_off, size, sets, ways, attr, line)
    return struct.pack('<BBHIIIIBBH', 1, CACHE_LEN, 0, 0, next_off, 0, 0, 0, 0, 0)


def node(flags, parent, acpi_id, resources):
    return struct.pack('<BBHIIII', 0, NODE_LEN + 4 * len(resources), 0, flags, parent, acpi_id,
                       len(resources)) + b''.join(struct.pack('<I', r) for r in resources)


def id_struct():
    return struct.pack('<BBHIQQHHH', 2, ID_LEN, 0, 0, 0, 0, 0, 0, 0)


def build(a55_uids, m3_uids, valid=False):
    """Layout (offsets from the table start):
    36  M3 L3 placeholder (next 0)
        M3 L2 (next L3), M3 L1I (next L2), M3 L1D (next L2)
        A55 L2 (next 0), A55 L1I (next L2), A55 L1D (next L2)
        ID structure
        package node (flags 1: physical package; resources: the ID structure)
        one node per core (flags 2: ACPI processor ID valid; parent: package; resources: its type's L1I, L1D)
    """
    body = bytearray()
    off = lambda: 36 + len(body)

    m3_l3 = off(); body += cache(0, 3, 2, False)
    m3_l2 = off(); body += cache(m3_l3, 2, 2, False)
    m3_l1i = off(); body += cache(m3_l2, 1, 1, False)
    m3_l1d = off(); body += cache(m3_l2, 1, 0, False)
    a55_l2 = off(); body += cache(0, 2, 2, valid, A55_GEOMETRY[(2, 2)])
    a55_l1i = off(); body += cache(a55_l2, 1, 1, valid, A55_GEOMETRY[(1, 1)])
    a55_l1d = off(); body += cache(a55_l2, 1, 0, valid, A55_GEOMETRY[(1, 0)])
    ident = off(); body += id_struct()
    pkg = off(); body += node(0x1, 0, 0, [ident])
    for uid in a55_uids:
        body += node(0x2, pkg, uid, [a55_l1i, a55_l1d])
    for uid in m3_uids:
        body += node(0x2, pkg, uid, [m3_l1i, m3_l1d])

    length = 36 + len(body)
    hdr = bytearray(struct.pack('<4sIBB6s8sI4sI', b'PPTT', length, REVISION, 0, OEM_ID, OEM_TABLE_ID,
                                OEM_REV, CREATOR_ID, CREATOR_REV))
    t = hdr + body
    t[9] = (-sum(t)) & 0xFF
    return bytes(t)


def decode(t):
    """Parse and check a PPTT; returns lines and raises ValueError on any inconsistency."""
    out = []
    if t[:4] != b'PPTT' or struct.unpack_from('<I', t, 4)[0] != len(t):
        raise ValueError('not a PPTT or length mismatch')
    rev = t[8]
    out.append(f'PPTT rev {rev} len {len(t)} oem {t[10:16]!r} {t[16:24]!r}'
               f'{"" if sum(t) & 0xFF == 0 else " (checksum not set: the ACPI table protocol sets it at install)"}')
    starts = {}
    o = 36
    while o < len(t):
        typ, ln = t[o], t[o + 1]
        if ln < 4 or o + ln > len(t):
            raise ValueError(f'bad structure length at {o:#x}')
        starts[o] = (typ, ln)
        o += ln
    caches = {k for k, v in starts.items() if v[0] == 1}
    for o, (typ, ln) in starts.items():
        if typ == 0:
            fl, par, uid, n = struct.unpack_from('<IIII', t, o + 4)
            if ln != NODE_LEN + 4 * n:
                raise ValueError(f'node {o:#x}: length {ln} for {n} resources')
            res = [struct.unpack_from('<I', t, o + 20 + 4 * i)[0] for i in range(n)]
            if par and (par not in starts or starts[par][0] != 0):
                raise ValueError(f'node {o:#x}: parent {par:#x} is not a node')
            for r in res:
                if r not in starts:
                    raise ValueError(f'node {o:#x}: resource {r:#x} points at nothing')
            out.append(f'{o:#05x} node flags={fl:#x} parent={par:#x} uid={uid} res={[hex(r) for r in res]}')
        elif typ == 1:
            if ln not in (24, 28):
                raise ValueError(f'cache {o:#x}: length {ln}')
            fl, nxt, size, sets, ways, attr, line = struct.unpack_from('<IIIIBBH', t, o + 4)
            if nxt and nxt not in caches:
                raise ValueError(f'cache {o:#x}: next level {nxt:#x} is not a cache')
            out.append(f'{o:#05x} cache flags={fl:#x} next={nxt:#x} size={size} sets={sets} ways={ways} '
                       f'attr={attr:#x} line={line}')
        elif typ == 2:
            out.append(f'{o:#05x} id')
        else:
            raise ValueError(f'unknown structure type {typ} at {o:#x}')
    # no cycles in the cache chains
    for c in caches:
        seen, x = set(), c
        while x:
            if x in seen:
                raise ValueError(f'cache chain loop at {c:#x}')
            seen.add(x)
            x = struct.unpack_from('<I', t, x + 8)[0]
    return out


def main():
    a = sys.argv[1:]
    if a[:1] == ['--decode'] and len(a) == 2:
        print('\n'.join(decode(open(a[1], 'rb').read())))
        return
    valid = '--valid' in a
    a = [x for x in a if x != '--valid']
    a55, m3 = [0, 1, 2, 3], [4, 5, 6, 7]
    if '--uids' in a:
        i = a.index('--uids')
        for spec in a[i + 1:i + 3]:
            k, v = spec.split('=')
            ids = [int(x, 0) for x in v.split(',') if x]
            if k == 'A55':
                a55 = ids
            elif k == 'M3':
                m3 = ids
            else:
                sys.exit(f'bad --uids part {spec}')
        del a[i:i + 3]
    if len(a) != 1:
        sys.exit(__doc__)
    t = build(a55, m3, valid)
    decode(t)
    assert sum(t) & 0xFF == 0
    open(a[0], 'wb').write(t)
    print(f'PPTT {len(t)} bytes, A55 UIDs {a55}, M3 UIDs {m3}, geometry {"A55 measured" if valid else "zero (OEM shape)"}')


if __name__ == '__main__':
    main()
