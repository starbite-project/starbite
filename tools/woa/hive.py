#!/usr/bin/env python3
"""Minimal read-only Windows registry hive reader (regf), no dependencies.

  hive.py HIVE 'Path\\To\\Key'     list values and subkeys of a key
  hive.py HIVE 'Path\\To\\Key' -r  recurse

Handles nk/vk/lf/lh/li/ri cells and big data (db). Never writes.
"""
import struct
import sys

B = 0x1000
TYPES = {0: 'NONE', 1: 'SZ', 2: 'EXPAND_SZ', 3: 'BINARY', 4: 'DWORD', 7: 'MULTI_SZ', 11: 'QWORD'}


class Hive:
    def __init__(self, path):
        self.d = open(path, 'rb').read()
        assert self.d[:4] == b'regf', 'not a hive'
        self.root = struct.unpack_from('<I', self.d, 0x24)[0]

    def cell(self, off):
        o = off + B
        size = -struct.unpack_from('<i', self.d, o)[0]
        return o + 4, size

    def nk(self, off):
        o, _ = self.cell(off)
        assert self.d[o:o + 2] == b'nk', f'no nk at {off:#x}'
        flags = struct.unpack_from('<H', self.d, o + 2)[0]
        nsub, = struct.unpack_from('<I', self.d, o + 0x14)
        sublist, = struct.unpack_from('<I', self.d, o + 0x1c)
        nval, vlist = struct.unpack_from('<II', self.d, o + 0x24)
        nlen, = struct.unpack_from('<H', self.d, o + 0x48)
        raw = self.d[o + 0x4c:o + 0x4c + nlen]
        name = raw.decode('latin-1') if flags & 0x20 else raw.decode('utf-16-le')
        return dict(name=name, nsub=nsub, sublist=sublist, nval=nval, vlist=vlist)

    def subkeys(self, k):
        if not k['nsub'] or k['sublist'] == 0xffffffff:
            return []
        out = []

        def walk(off):
            o, _ = self.cell(off)
            sig = self.d[o:o + 2]
            n, = struct.unpack_from('<H', self.d, o + 2)
            if sig in (b'lf', b'lh'):
                for i in range(n):
                    out.append(struct.unpack_from('<I', self.d, o + 4 + 8 * i)[0])
            elif sig == b'li':
                for i in range(n):
                    out.append(struct.unpack_from('<I', self.d, o + 4 + 4 * i)[0])
            elif sig == b'ri':
                for i in range(n):
                    walk(struct.unpack_from('<I', self.d, o + 4 + 4 * i)[0])
        walk(k['sublist'])
        return [(off, self.nk(off)) for off in out]

    def values(self, k):
        res = []
        if not k['nval'] or k['vlist'] == 0xffffffff:
            return res
        lo, _ = self.cell(k['vlist'])
        for i in range(k['nval']):
            vo, _ = self.cell(struct.unpack_from('<I', self.d, lo + 4 * i)[0])
            assert self.d[vo:vo + 2] == b'vk'
            nlen, dlen, doff, typ, flags = struct.unpack_from('<HIIIH', self.d, vo + 2)
            raw = self.d[vo + 0x14:vo + 0x14 + nlen]
            name = (raw.decode('latin-1') if flags & 1 else raw.decode('utf-16-le')) or '(default)'
            if dlen & 0x80000000:
                data = struct.pack('<I', doff)[:dlen & 0x7fffffff]
            else:
                co, csize = self.cell(doff)
                if self.d[co:co + 2] == b'db' and dlen > 16344:
                    n, seglist = struct.unpack_from('<HI', self.d, co + 2)
                    so, _ = self.cell(seglist)
                    data = b''
                    for j in range(n):
                        sco, ssize = self.cell(struct.unpack_from('<I', self.d, so + 4 * j)[0])
                        data += self.d[sco:sco + ssize - 4]
                    data = data[:dlen]
                else:
                    data = self.d[co:co + dlen]
            res.append((name, typ, data))
        return res

    def find(self, path):
        off = self.root
        k = self.nk(off)
        for part in [p for p in path.split('\\') if p]:
            for so, sk in self.subkeys(k):
                if sk['name'].lower() == part.lower():
                    off, k = so, sk
                    break
            else:
                raise SystemExit(f'key not found: {part}')
        return off, k


def fmt(typ, data):
    if typ == 4 and len(data) == 4:
        v = struct.unpack('<I', data)[0]
        return f'0x{v:x} ({v})'
    if typ == 11 and len(data) == 8:
        return f'0x{struct.unpack("<Q", data)[0]:x}'
    if typ in (1, 2):
        return repr(data.decode('utf-16-le', 'replace').rstrip('\0'))
    if typ == 7:
        return repr([s for s in data.decode('utf-16-le', 'replace').split('\0') if s])
    return data[:64].hex() + ('...' if len(data) > 64 else '')


def dump(h, k, path, rec, depth=0):
    print(f'[{path}]')
    for name, typ, data in h.values(k):
        print(f'  {name} = {TYPES.get(typ, typ)} {fmt(typ, data)}')
    subs = h.subkeys(k)
    if not rec:
        for _, sk in subs:
            print(f'  <{sk["name"]}>')
    else:
        for _, sk in subs:
            dump(h, sk, path + '\\' + sk['name'], rec, depth + 1)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    h = Hive(sys.argv[1])
    _, k = h.find(sys.argv[2])
    dump(h, k, sys.argv[2], '-r' in sys.argv[3:])


if __name__ == '__main__':
    main()
