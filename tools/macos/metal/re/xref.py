#!/usr/bin/env python3
"""Small, low-memory reverse-engineering helpers for dylibs extracted from a dyld shared cache
(`ipsw dsc extract ... --slide`): find strings, find ADRP+ADD/LDR references to an address, and
name the function a reference sits in (nearest preceding symbol from LC_SYMTAB / exports).

  xref.py str  DYLIB 'text'            -> vmaddr of every C string containing text
  xref.py refs DYLIB 0xADDR [0xADDR..] -> code addresses that compute ADDR (adrp+add, adrp+ldr)
  xref.py sym  DYLIB 0xADDR            -> nearest symbol at or below ADDR
  xref.py find DYLIB 'text'            -> str + refs + sym in one go

Reads the file with mmap; nothing is kept but the text section's words as needed.
"""
import mmap
import struct
import sys

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x2


class MachO:
    def __init__(self, path):
        f = open(path, 'rb')
        self.m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        magic, cpu, sub, ftype, ncmds, sizeof, flags, _ = struct.unpack_from('<IiiIIIII', self.m, 0)
        assert magic == 0xfeedfacf, 'not a thin 64-bit Mach-O'
        self.sects = []
        self.segs = []
        self.symtab = None
        off = 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', self.m, off)
            if cmd == LC_SEGMENT_64:
                segname = self.m[off + 8:off + 24].rstrip(b'\0').decode()
                vmaddr, vmsize, fileoff, filesize = struct.unpack_from('<QQQQ', self.m, off + 24)
                nsects = struct.unpack_from('<I', self.m, off + 64)[0]
                self.segs.append((segname, vmaddr, vmsize, fileoff, filesize))
                so = off + 72
                for _ in range(nsects):
                    sname = self.m[so:so + 16].rstrip(b'\0').decode()
                    addr, ssize, soff = struct.unpack_from('<QQI', self.m, so + 32)
                    self.sects.append((segname, sname, addr, ssize, soff))
                    so += 80
            elif cmd == LC_SYMTAB:
                self.symtab = struct.unpack_from('<IIII', self.m, off + 8)
            off += size

    def sect(self, seg, name):
        for s in self.sects:
            if s[0] == seg and s[1] == name:
                return s
        return None

    def off_of(self, addr):
        for seg, vmaddr, vmsize, fileoff, filesize in self.segs:
            if vmaddr <= addr < vmaddr + filesize:
                return fileoff + addr - vmaddr
        return None

    def strings(self, text):
        t = text.encode()
        out = []
        for seg, name, addr, size, off in self.sects:
            if name not in ('__cstring', '__oslogstring', '__const', '__objc_methname', '__objc_classname'):
                continue
            # extracted cache images keep the section's vmaddr; file offsets come from the segments
            foff = self.off_of(addr)
            if foff is None:
                continue
            data = self.m[foff:foff + size]
            i = data.find(t)
            while i >= 0:
                s = data.rfind(b'\0', 0, i) + 1
                e = data.find(b'\0', i)
                out.append((addr + s, data[s:e].decode('utf-8', 'replace')))
                i = data.find(t, e)
        return out

    def cfstrings(self, addrs):
        """CFString constants (isa, flags, ptr, len) whose C string is one of addrs."""
        addrs = set(addrs)
        out = []
        for seg, name, addr, size, off in self.sects:
            if name != '__cfstring':
                continue
            foff = self.off_of(addr)
            if foff is None:
                continue
            for i in range(0, size - 31, 32):
                p = struct.unpack_from('<Q', self.m, foff + i + 16)[0]
                for cand in (p, 0x180000000 + (p & 0x3ffffffff)):
                    if cand in addrs:
                        out.append((addr + i, cand))
                        break
        return out

    def symbols(self):
        if not self.symtab:
            return []
        symoff, nsyms, stroff, strsize = self.symtab
        syms = []
        for i in range(nsyms):
            n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from('<IBBHQ', self.m, symoff + i * 16)
            if n_type & 0xe0 or n_value == 0:     # stabs
                continue
            if (n_type & 0x0e) != 0x0e:            # N_SECT only
                continue
            e = self.m.find(b'\0', stroff + n_strx)
            syms.append((n_value, self.m[stroff + n_strx:e].decode('utf-8', 'replace')))
        syms.sort()
        return syms

    def refs(self, targets):
        targets = set(targets)
        pages = {t & ~0xfff for t in targets}
        sec = self.sect('__TEXT', '__text')
        _, _, addr, size, _ = sec
        foff = self.off_of(addr)
        hits = []
        words = struct.unpack_from('<%dI' % (size // 4), self.m, foff)
        for i, w in enumerate(words):
            if (w & 0x9f000000) != 0x90000000:   # ADRP
                continue
            pc = addr + i * 4
            imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2)
            if imm & (1 << 20):
                imm -= 1 << 21
            page = (pc & ~0xfff) + (imm << 12)
            if page not in pages:
                continue
            rd = w & 31
            for j in range(i + 1, min(i + 8, len(words))):
                v = words[j]
                if (v & 0xffc00000) == 0x91000000 and ((v >> 5) & 31) == rd:        # ADD imm
                    t = page + ((v >> 10) & 0xfff)
                elif (v & 0xffc00000) == 0xf9400000 and ((v >> 5) & 31) == rd:      # LDR x, [rd, #imm*8]
                    t = page + ((v >> 10) & 0xfff) * 8
                elif (v & 0xffc00000) == 0xb9400000 and ((v >> 5) & 31) == rd:      # LDR w
                    t = page + ((v >> 10) & 0xfff) * 4
                else:
                    continue
                if t in targets:
                    hits.append((pc, t))
                break
        return hits


def nearest(syms, a):
    lo, hi = 0, len(syms) - 1
    best = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if syms[mid][0] <= a:
            best = syms[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    return best


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    m = MachO(path)
    if cmd == 'str':
        for a, s in m.strings(sys.argv[3]):
            print('%#x %r' % (a, s))
    elif cmd == 'refs':
        syms = m.symbols()
        for pc, t in m.refs([int(x, 16) for x in sys.argv[3:]]):
            s = nearest(syms, pc)
            print('%#x -> %#x  in %s+%#x' % (pc, t, s[1] if s else '?', pc - s[0] if s else 0))
    elif cmd == 'sym':
        s = nearest(m.symbols(), int(sys.argv[3], 16))
        print(s)
    elif cmd == 'find':
        syms = m.symbols()
        found = m.strings(sys.argv[3])
        for a, s in found:
            print('%#x %r' % (a, s))
        cfs = m.cfstrings([a for a, _ in found])
        for cf, a in cfs:
            print('%#x CFString of %#x' % (cf, a))
        for pc, t in m.refs([a for a, _ in found] + [cf for cf, _ in cfs]):
            s = nearest(syms, pc)
            print('  ref %#x -> %#x  in %s+%#x' % (pc, t, s[1] if s else '?', pc - s[0] if s else 0))


if __name__ == '__main__':
    main()
