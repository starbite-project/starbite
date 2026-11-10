#!/usr/bin/env python3
"""Count ARMv8.1 LSE atomics and ARMv8.3 LDAPR in ARM64 PE files (docs/WIN11-M3-CLEAN.md section 25).

  lse-scan.py FILE_OR_DIR...

The Exynos M3 is ARMv8.0: any LSE instruction it executes is an undefined instruction (c000001d). A module whose
code sections contain LSE encodings either checks the CPU first (outline atomics, IsProcessorFeaturePresent) or
crashes on the M3 when it reaches them (winsqlite3.dll 3.51.1.0: CASAL at +0xEC614, no check). This tool only counts
encodings in executable sections (CAS/CASP and the LD<op>/SWP family); a non-zero count means "look closer", not
"crashes". Read-only.
"""
import os
import struct
import sys


def scan(path):
    d = open(path, 'rb').read()
    if d[:2] != b'MZ':
        return None
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    if d[pe:pe + 4] != b'PE\0\0' or struct.unpack_from('<H', d, pe + 4)[0] != 0xAA64:
        return None
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    osz = struct.unpack_from('<H', d, pe + 20)[0]
    hits = {'cas': 0, 'casp': 0, 'ldop/swp': 0, 'ldapr': 0}
    first = None
    for i in range(nsec):
        o = pe + 24 + osz + 40 * i
        vs, va, rs, ra, = struct.unpack_from('<IIII', d, o + 8)
        ch = struct.unpack_from('<I', d, o + 36)[0]
        if not ch & 0x20000000:                     # IMAGE_SCN_MEM_EXECUTE
            continue
        for off in range(ra, ra + min(rs, vs) - 3, 4):
            w = struct.unpack_from('<I', d, off)[0]
            kind = None
            if w & 0x3FFFFC00 == 0x38BFC000:
                kind = 'ldapr'                      # ARMv8.3 RCpc load-acquire (not on the M3 either)
            elif w & 0x3FA07C00 == 0x08A07C00:
                kind = 'cas'
            elif w & 0xBFA07C00 == 0x08207C00:
                kind = 'casp'
            elif w & 0x3F200C00 == 0x38200000:
                kind = 'ldop/swp'
            if kind:
                hits[kind] += 1
                if first is None:
                    first = va + (off - ra)
    return hits, first


def main():
    for arg in sys.argv[1:]:
        paths = [arg] if os.path.isfile(arg) else [os.path.join(r, f) for r, _, fs in os.walk(arg) for f in fs
                                                     if f.lower().endswith(('.dll', '.exe', '.sys'))]
        for p in sorted(paths):
            try:
                r = scan(p)
            except (OSError, struct.error):
                continue
            if r and sum(r[0].values()):
                h, first = r
                print(f'{sum(h.values()):6d}  cas {h["cas"]} casp {h["casp"]} ldop/swp {h["ldop/swp"]} ldapr {h["ldapr"]}  first +{first:#x}  {p}')


if __name__ == '__main__':
    main()
