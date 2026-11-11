#!/usr/bin/env python3
"""Recover what a prelinked kext in an Apple boot kernel collection (MH_FILESET) imports (plan H5).

A kext inside a kernelcache has no undefined symbols left: kmutil bound every import to an address.
This walks the collection's chained fixups (DYLD_CHAINED_PTR_64_KERNEL_CACHE) and the kext's direct
BL calls, and maps every target outside the kext back to an exported symbol of the kernel or of
another kext in the same collection (macOS boot KCs keep those exports for aux-KC linking).

  fileset-imports.py KC ENTRY_ID OUT.json      e.g. ... boot.kc com.apple.filesystems.apfs apfs.json
  fileset-imports.py KC --symbolsets OUT.plist dump the kernel's __LINKINFO,__symbolsets
  fileset-imports.py KC --entries              list fileset entries

Read-only on the KC. Apple binaries stay under build/ (never in git)."""
import bisect, json, plistlib, struct, sys
from collections import defaultdict

LC_SEGMENT_64, LC_SYMTAB, LC_FILESET_ENTRY, LC_CHAINED = 0x19, 0x2, 0x80000035, 0x80000034


def load_cmds(d, off):
    ncmds, sz = struct.unpack_from('<II', d, off + 16)
    p = off + 32
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<II', d, p)
        yield cmd, p, csz
        p += csz


def segments(d, off):
    out = []
    for cmd, p, _ in load_cmds(d, off):
        if cmd == LC_SEGMENT_64:
            name = d[p + 8:p + 24].rstrip(b'\0').decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', d, p + 24)
            nsect = struct.unpack_from('<I', d, p + 64)[0]
            sects = []
            for s in range(nsect):
                so = p + 72 + s * 80
                sname = d[so:so + 16].rstrip(b'\0').decode()
                addr, size, foff = struct.unpack_from('<QQI', d, so + 32)
                flags = struct.unpack_from('<I', d, so + 64)[0]
                sects.append((sname, addr, size, foff, flags))
            out.append((name, vmaddr, vmsize, fileoff, filesize, sects))
    return out


class KC:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        d = self.d
        self.segs = segments(d, 0)
        self.base = min(s[1] for s in self.segs if s[2])
        self.entries = []
        for cmd, p, _ in load_cmds(d, 0):
            if cmd == LC_FILESET_ENTRY:
                vmaddr, fileoff, idoff = struct.unpack_from('<QQI', d, p + 8)
                eid = d[p + idoff:d.index(b'\0', p + idoff)].decode()
                self.entries.append((eid, vmaddr, fileoff))

    def v2f(self, va):
        for _, vm, vs, fo, fs, _ in self.segs:
            if vm <= va < vm + fs:
                return fo + va - vm
        return None

    def entry(self, eid):
        for e in self.entries:
            if e[0] == eid:
                return e
        raise SystemExit(f'no entry {eid}')

    def symbols(self):
        """addr -> (name, entry) for every defined symbol of every entry (externals preferred)."""
        d = self.d
        best = {}
        for eid, _, fo in self.entries:
            for cmd, p, _ in load_cmds(d, fo):
                if cmd != LC_SYMTAB:
                    continue
                symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', d, p + 8)
                for i in range(nsyms):
                    strx, typ, sect, desc, val = struct.unpack_from('<IBBHQ', d, symoff + i * 16)
                    if typ & 0xe0 or (typ & 0x0e) != 0x0e or not val:
                        continue
                    name = d[stroff + strx:d.index(b'\0', stroff + strx)].decode(errors='replace')
                    ext = bool(typ & 1)
                    old = best.get(val)
                    if old is None or (ext and not old[2]):
                        best[val] = (name, eid, ext)
        return best

    def fixups(self):
        """Yield (location va, target va, isAuth) for every chained fixup in the collection."""
        d = self.d
        for cmd, p, _ in load_cmds(d, 0):
            if cmd != LC_CHAINED:
                continue
            dataoff, _ = struct.unpack_from('<II', d, p + 8)
            starts_off = struct.unpack_from('<I', d, dataoff + 4)[0]
            si = dataoff + starts_off
            nseg = struct.unpack_from('<I', d, si)[0]
            for s in range(nseg):
                so = struct.unpack_from('<I', d, si + 4 + 4 * s)[0]
                if not so:
                    continue
                q = si + so
                _, page_size, fmt, seg_off, _, page_count = struct.unpack_from('<IHHQIH', d, q)
                assert fmt in (8, 11), f'pointer format {fmt}'
                for pg in range(page_count):
                    start = struct.unpack_from('<H', d, q + 22 + 2 * pg)[0]
                    if start == 0xFFFF:
                        continue
                    assert not start & 0x8000, 'multi-start pages not handled'
                    va = self.base + seg_off + pg * page_size + start
                    while True:
                        v = struct.unpack_from('<Q', d, self.v2f(va))[0]
                        target = v & 0x3FFFFFFF
                        level = (v >> 30) & 3
                        nxt = (v >> 51) & 0xFFF
                        auth = v >> 63
                        if level == 0:
                            yield va, self.base + target, auth
                        if not nxt:
                            break
                        va += nxt * 4


def imports(kc, eid):
    _, evm, efo = kc.entry(eid)
    esegs = segments(kc.d, efo)
    ranges = [(s[1], s[1] + s[2]) for s in esegs if s[2] and s[0] != '__LINKEDIT']
    inside = lambda a: any(lo <= a < hi for lo, hi in ranges)
    syms = kc.symbols()
    addrs = sorted(syms)
    entry_ranges = []
    for e, _, fo in kc.entries:
        for s in segments(kc.d, fo):
            if s[2] and s[0] != '__LINKEDIT':
                entry_ranges.append((s[1], s[1] + s[2], e))
    entry_ranges.sort()

    def owner(a):
        for lo, hi, e in entry_ranges:
            if lo <= a < hi:
                return e
        return '?'

    def name(a):
        if a in syms:
            return syms[a][0], 0, syms[a][1]
        i = bisect.bisect_right(addrs, a) - 1
        if i >= 0 and a - addrs[i] < 0x100:
            n, e, _ = syms[addrs[i]]
            if owner(addrs[i]) == owner(a):
                return n, a - addrs[i], e
        return None, 0, owner(a)

    out = defaultdict(lambda: {'refs': 0, 'kinds': set(), 'addends': set()})
    unnamed = defaultdict(int)
    for loc, tgt, auth in kc.fixups():
        if inside(loc) and not inside(tgt):
            n, add, e = name(tgt)
            if n is None:
                unnamed[e] += 1
                continue
            r = out[(n, e)]
            r['refs'] += 1; r['kinds'].add('ptr'); r['addends'].add(add)
    for sname, addr, size, foff, flags in (s for seg in esegs for s in seg[5]):
        if not flags & 0x80000000:
            continue
        for i in range(0, size & ~3, 4):
            w = struct.unpack_from('<I', kc.d, foff + i)[0]
            if (w & 0x7C000000) == 0x14000000:          # B / BL imm26
                imm = w & 0x3FFFFFF
                if imm & 0x2000000:
                    imm -= 0x4000000
                tgt = addr + i + imm * 4
                if not inside(tgt):
                    n, add, e = name(tgt)
                    if n is None:
                        unnamed[e] += 1
                        continue
                    r = out[(n, e)]
                    r['refs'] += 1; r['kinds'].add('call'); r['addends'].add(add)
    res = [{'symbol': n, 'provider': e, 'refs': v['refs'], 'kinds': sorted(v['kinds']),
            'addends': sorted(v['addends'])} for (n, e), v in sorted(out.items())]
    return res, dict(unnamed)


def symbolsets(kc):
    _, _, fo = kc.entry('com.apple.kernel')
    for s in segments(kc.d, fo):
        for sname, addr, size, foff, _ in s[5]:
            if sname == '__symbolsets':
                return kc.d[foff:foff + size]
    raise SystemExit('kernel has no __LINKINFO,__symbolsets')


if __name__ == '__main__':
    kc = KC(sys.argv[1])
    if sys.argv[2] == '--entries':
        for e in kc.entries:
            print(e[0], hex(e[1]))
    elif sys.argv[2] == '--symbolsets':
        open(sys.argv[3], 'wb').write(symbolsets(kc).rstrip(b'\0'))
    else:
        res, unnamed = imports(kc, sys.argv[2])
        json.dump({'entry': sys.argv[2], 'imports': res, 'unnamed_targets_by_owner': unnamed},
                  open(sys.argv[3], 'w'), indent=1)
        print(f'{len(res)} named imports; unnamed targets: {unnamed}')
