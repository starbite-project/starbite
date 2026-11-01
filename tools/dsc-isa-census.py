#!/usr/bin/env python3
"""Count instructions in an arm64e dyld shared cache that cores without
ARMv8.3 (the S9: A55 v8.2, Mongoose M3 v8.0) cannot execute.

usage: dsc-isa-census.py DIR/dyld_shared_cache_arm64e [IMAGE-SUBSTRING ...]
Scans only S_ATTR_PURE_INSTRUCTIONS sections of each image, so string and
constant data in __TEXT does not count."""
import glob, os, struct, sys
import numpy as np

main = sys.argv[1]
want = sys.argv[2:]
maps = []                                  # (addr, size, path, fileoff)
for p in sorted(glob.glob(main + '*')):
    with open(p, 'rb') as f:
        h = f.read(0x200)
    if not h.startswith(b'dyld_v1'):
        continue
    mo, mc = struct.unpack_from('<II', h, 0x10)
    with open(p, 'rb') as f:
        f.seek(mo)
        for i in range(mc):
            a, s, o, _, _ = struct.unpack('<QQQII', f.read(32))
            maps.append((a, s, p, o))
fhs = {}
def read(addr, n):
    for a, s, p, o in maps:
        if a <= addr < a + s:
            fh = fhs.get(p) or fhs.setdefault(p, open(p, 'rb'))
            fh.seek(o + addr - a)
            return fh.read(n)
    return None

with open(main, 'rb') as f:
    h = f.read(0x200)
io, ic = struct.unpack_from('<II', h, 0x1c0)   # imagesOffset/Count (newer caches)
if not ic:
    io, ic = struct.unpack_from('<II', h, 0x18)
with open(main, 'rb') as f:
    f.seek(io); imgs = [struct.unpack('<QQQII', f.read(32)) for _ in range(ic)]
    paths = []
    for im in imgs:
        f.seek(im[3]); paths.append(f.read(256).split(b'\0')[0].decode())

# name: (mask, value).  Order matters: first match wins.
PATS = [
 ('RETAA/RETAB',        0xFFFFFBFF, 0xD65F0BFF),
 ('BR/BLR auth',        0xFE9FF800, 0xD61F0800),
 ('LDRAA/LDRAB',        0xFF200400, 0xF8200400),
 ('PAC*/AUT*/XPAC reg', 0xFFFF8000, 0xDAC10000),
 ('PACGA',              0xFFE0FC00, 0x9AC03000),
 ('ERETAA/B',           0xFFFFFBFF, 0xD69F0BFF),
 ('FJCVTZS (JSCVT)',    0xFFFFFC00, 0x1E7E0000),
 ('FCMLA/FCADD',        0x9F20E400, 0x2E00C400),
 ('FCADD',              0xBF20EC00, 0x2E00E400),
 ('LDAPR (RCpc)',       0x3FFFFC00, 0x38BFC000),
 ('LDAPUR/STLUR v8.4',  0x3F000C00, 0x19000000),
 ('LSE CAS',            0x3FA07C00, 0x08A07C00),
 ('LSE CASP',           0xBFA07C00, 0x08207C00),
 ('LSE LD<op>/SWP',     0x3F200C00, 0x38200000),
 ('DotProd',            0xBFE0FC00, 0x0E809400),
 ('DotProd U',          0xBFE0FC00, 0x2E809400),
 ('DotProd idx',        0x9FC0F400, 0x0F80E000),
 ('SQRDMLAH/SH (RDM)',  0xBF20F400, 0x2E008400),
 ('FP16 scalar dp',     0xFF200000, 0x1EE00000),
 ('hint PAC (NOP here)',0xFFFFFD1F, 0xD503211F),
]
masks = np.array([m for _, m, _ in PATS], dtype=np.uint32)
vals = np.array([v for _, _, v in PATS], dtype=np.uint32)
tot = np.zeros(len(PATS), dtype=np.int64); words = 0; per = []
for im, path in zip(imgs, paths):
    if want and not any(w in path for w in want):
        continue
    mh = read(im[0], 32)
    if not mh or struct.unpack_from('<I', mh)[0] != 0xfeedfacf:
        continue
    ncmds, sz = struct.unpack_from('<II', mh, 16)
    lc = read(im[0] + 32, sz); off = 0; cnt = np.zeros(len(PATS), dtype=np.int64); n = 0
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', lc, off)
        if cmd == 0x19:
            nsect = struct.unpack_from('<I', lc, off + 64)[0]
            for s in range(nsect):
                so = off + 72 + s * 80
                addr, size = struct.unpack_from('<QQ', lc, so + 32)
                flags = struct.unpack_from('<I', lc, so + 64)[0]
                if flags & 0x80000000 and size:
                    b = read(addr, size)
                    if not b: continue
                    w = np.frombuffer(b[:len(b) & ~3], dtype=np.uint32)
                    n += w.size; left = np.ones(w.size, bool)
                    for i in range(len(PATS)):
                        hit = left & ((w & masks[i]) == vals[i])
                        cnt[i] += hit.sum(); left &= ~hit
        off += csz
    tot += cnt; words += n; per.append((cnt[:6].sum(), n, path))
print(f'images {len(per)}  instructions {words:,}  ({words*4/2**20:.0f} MiB of code)')
for (nm, _, _), c in zip(PATS, tot):
    print(f'  {nm:22s} {c:12,d}  {c/words*1e3:8.3f} per 1000')
print('top images by non-hint PAC ops:')
for c, n, p in sorted(per, reverse=True)[:8]:
    print(f'  {c:9,d} / {n:11,d}  {p}')
