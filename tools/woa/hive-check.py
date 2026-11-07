#!/usr/bin/env python3
"""Deep consistency check of a Windows registry hive file (regf), read-only, modelled on what Windows'
CmCheckRegistry verifies when it loads a hive: bins and cells, every key's parent link, subkey lists (lf/lh/li/ri:
counts, allocation, sort order, lh hashes), value lists and data cells (incl. big data 'db'), class names, and the
security descriptor list (sk flink/blink ring and reference counts).

  hive-check.py HIVE [HIVE...]      prints problems per hive; exit 1 if any
"""
import struct
import sys

B = 0x1000


class Check:
    def __init__(self, path):
        self.path = path
        self.d = open(path, 'rb').read()
        self.p = []
        d = self.d
        assert d[:4] == b'regf', 'no regf'
        s1, s2 = struct.unpack_from('<II', d, 4)
        if s1 != s2:
            self.p.append(f'header dirty seq {s1}/{s2}')
        self.root = struct.unpack_from('<I', d, 0x24)[0]
        self.hlen = struct.unpack_from('<I', d, 0x28)[0]
        self.cells = {}                       # cell offset -> (size, allocated)
        off = B
        while off < B + self.hlen:
            if d[off:off + 4] != b'hbin':
                self.p.append(f'no hbin at {off - B:#x}')
                break
            bsz = struct.unpack_from('<I', d, off + 8)[0]
            c = off + 0x20
            while c < off + bsz:
                sz = struct.unpack_from('<i', d, c)[0]
                a = abs(sz)
                if a < 8 or a % 8 or c + a > off + bsz:
                    self.p.append(f'bad cell size at {c - B:#x}')
                    break
                self.cells[c - B] = (a, sz < 0)
                c += a
            off += bsz
        self.sk_refs = {}
        self.keys = 0
        self.values = 0
        self.seen = set()

    def cell(self, off, what, sig=None, need=0):
        """data start of an allocated cell, or None (problem recorded)"""
        if off not in self.cells:
            self.p.append(f'{what}: {off:#x} is not a cell start')
            return None
        size, alloc = self.cells[off]
        if not alloc:
            self.p.append(f'{what}: cell {off:#x} is free')
            return None
        if need and size - 4 < need:
            self.p.append(f'{what}: cell {off:#x} too small ({size - 4} < {need})')
            return None
        o = off + B + 4
        if sig and self.d[o:o + 2] != sig:
            self.p.append(f'{what}: cell {off:#x} sig {self.d[o:o + 2]!r} != {sig!r}')
            return None
        return o

    def name(self, o, nlen_off, name_off, flag_bit, flags):
        n = struct.unpack_from('<H', self.d, o + nlen_off)[0]
        raw = self.d[o + name_off:o + name_off + n]
        return raw.decode('latin-1') if flags & flag_bit else raw.decode('utf-16-le', 'replace')

    def list_entries(self, off, what, depth=0):
        o = self.cell(off, what)
        if o is None:
            return []
        sig = self.d[o:o + 2]
        n = struct.unpack_from('<H', self.d, o + 2)[0]
        if sig in (b'lf', b'lh'):
            if self.cells[off][0] - 4 < 4 + 8 * n:
                self.p.append(f'{what}: {sig!r} count {n} overflows cell')
                return []
            return [(struct.unpack_from('<II', self.d, o + 4 + 8 * i), sig) for i in range(n)]
        if sig == b'li':
            return [((struct.unpack_from('<I', self.d, o + 4 + 4 * i)[0], None), sig) for i in range(n)]
        if sig == b'ri' and depth == 0:
            out = []
            for i in range(n):
                out += self.list_entries(struct.unpack_from('<I', self.d, o + 4 + 4 * i)[0], what + '/ri', 1)
            return out
        self.p.append(f'{what}: bad list sig {sig!r}')
        return []

    @staticmethod
    def lh_hash(name):
        h = 0
        for ch in name.upper():
            h = (h * 37 + ord(ch)) & 0xffffffff
        return h

    def key(self, off, parent, path):
        if off in self.seen:
            self.p.append(f'{path}: key cell {off:#x} reached twice (loop)')
            return
        self.seen.add(off)
        o = self.cell(off, path, b'nk', 0x4c)
        if o is None:
            return
        self.keys += 1
        d = self.d
        flags = struct.unpack_from('<H', d, o + 2)[0]
        par = struct.unpack_from('<I', d, o + 0x10)[0]
        nsub = struct.unpack_from('<I', d, o + 0x14)[0]
        sublist = struct.unpack_from('<I', d, o + 0x1c)[0]
        nval, vlist, sk, cls = struct.unpack_from('<IIII', d, o + 0x24)
        clen = struct.unpack_from('<H', d, o + 0x4a)[0]
        nm = self.name(o, 0x48, 0x4c, 0x20, flags)
        if parent is not None and par != parent:
            self.p.append(f'{path}: parent {par:#x} != {parent:#x}')
        # security
        if self.cell(sk, path + ' [sk]', b'sk', 0x14) is not None:
            self.sk_refs[sk] = self.sk_refs.get(sk, 0) + 1
        if clen and self.cell(cls, path + ' [class]', None, clen) is None:
            pass
        # values
        if nval:
            lo = self.cell(vlist, path + ' [values]', None, 4 * nval)
            if lo is not None:
                for i in range(nval):
                    voff = struct.unpack_from('<I', d, lo + 4 * i)[0]
                    vo = self.cell(voff, f'{path} [value {i}]', b'vk', 0x14)
                    if vo is None:
                        continue
                    self.values += 1
                    dlen, doff = struct.unpack_from('<II', d, vo + 4)
                    if dlen & 0x80000000 or dlen == 0:
                        continue
                    if dlen > 16344 and self.d[self.cell(doff, f'{path} [value {i} data]') or 0:][:2] == b'db':
                        dbo = self.cell(doff, f'{path} [value {i} db]', b'db', 8)
                        if dbo is not None:
                            nseg, segl = struct.unpack_from('<HI', d, dbo + 2)
                            so = self.cell(segl, f'{path} [value {i} db list]', None, 4 * nseg)
                            if so is not None:
                                for s in range(nseg):
                                    self.cell(struct.unpack_from('<I', d, so + 4 * s)[0], f'{path} [value {i} seg {s}]')
                    else:
                        self.cell(doff, f'{path} [value {i} data]', None, dlen)
        # subkeys
        if nsub:
            ents = self.list_entries(sublist, path + ' [subkeys]')
            if len(ents) != nsub:
                self.p.append(f'{path}: subkey count {nsub} but list has {len(ents)}')
            prev = None
            for (koff, hint), sig in ents:
                ko = self.cell(koff, f'{path} [subkey]', b'nk', 0x4c)
                if ko is None:
                    continue
                kflags = struct.unpack_from('<H', d, ko + 2)[0]
                kn = self.name(ko, 0x48, 0x4c, 0x20, kflags)
                if sig == b'lh' and hint != self.lh_hash(kn):
                    self.p.append(f'{path}\\{kn}: lh hash {hint:#x} != {self.lh_hash(kn):#x}')
                if sig == b'lf' and d[ko + 0x4c:ko + 0x4c + 4][:min(4, len(kn))] and False:
                    pass
                up = kn.upper()
                if prev is not None and up <= prev:
                    self.p.append(f'{path}: subkeys out of order: {prev!r} then {up!r}')
                prev = up
                self.key(koff, off, f'{path}\\{kn}')

    def run(self):
        self.key(self.root, None, '')
        # security descriptor ring + reference counts
        for sk, refs in self.sk_refs.items():
            o = self.cells and (sk + B + 4)
            fl, bl, rc = struct.unpack_from('<III', self.d, o + 4)
            if rc != refs:
                self.p.append(f'sk {sk:#x}: refcount {rc} but {refs} keys use it')
            for nb, what in ((fl, 'flink'), (bl, 'blink')):
                if self.cell(nb, f'sk {sk:#x} {what}', b'sk') is None:
                    continue
            fo = fl + B + 4
            if struct.unpack_from('<I', self.d, fo + 8)[0] != sk:
                self.p.append(f'sk {sk:#x}: flink {fl:#x} does not point back')
        return self.p


def main():
    bad = 0
    for path in sys.argv[1:]:
        c = Check(path)
        p = c.run()
        print(f'{path}: {c.keys} keys, {c.values} values, {len(c.sk_refs)} sk, {len(p)} problems')
        for x in p[:25]:
            print('   ', x)
        bad += bool(p)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
