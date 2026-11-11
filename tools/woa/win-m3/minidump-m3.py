#!/usr/bin/env python3
"""T3 host parser (docs/WIN11-M3-X86.md section 3): what an emulated process faulted on, from a WER LocalDumps .dmp.

  minidump-m3.py FILE.dmp [--all-threads]

Reads the streams SystemInfo(7), Exception(6), ThreadList(3), ModuleList(4), MemoryList(5), Memory64List(9) and
MemoryInfoList(16). Prints:
  1. the exception stream: code, address, parameters, the faulting thread, and that thread's context;
  2. the thread that stopped inside xtajit64 (+0x36D6C .. fail-fast) or xtajit (+0x5EF68), or failing that the
     exception thread: x19 -> EXCEPTION_RECORD (code/address), x25 -> CONTEXT.Pc (+0x108), the DWORD at x29+0x14;
  3. for every interesting address (exception address, record address, CONTEXT.Pc): the region (module+RVA,
     private executable = JIT output, mapped executable = XtaCache section) and the 32-bit word there, classified
     (LSE, LDAPR/RCPC, RCPC2, RDM, DOT, FP16, ... via m3-scan.py's classifier when available, else a built-in
     LSE/RCPC subset).

Thread contexts: ARM64 (ContextFlags bit 0x00400000) is decoded natively. An x64 process on ARM64 is ARM64EC; WER can
store its contexts in AMD64 layout (bit 0x00100000). Those are mapped back with the ARM64EC register map
(x19=R12, x25=Rsi, x29=Rbp, sp=Rsp, pc=Rip, ...) and labelled 'EC'. Read-only; host only.
"""
import importlib.util
import os
import struct
import sys

M3SCAN = os.environ.get('M3SCAN', '/tmp/scratch/'
                        '5518f30c-0697-4188-8d38-a65e4dd4699c/scratchpad/x86/m3-scan.py')
STOPS = {'xtajit64.dll': [(0x36910, 0x36E00, 'exception handler / fail-fast 5 at +0x36D6C')],
         'xtajit.dll': [(0x5EF00, 0x5F000, 'fail-fast at +0x5EF68')]}
CODES = {0xC000001D: 'ILLEGAL_INSTRUCTION', 0xC0000005: 'ACCESS_VIOLATION', 0xC0000409: 'STACK_BUFFER_OVERRUN/fail-fast',
         0x80000003: 'BREAKPOINT', 0xC0000096: 'PRIVILEGED_INSTRUCTION', 0xC0000602: 'FAIL_FAST_EXCEPTION',
         0xC0000094: 'INT_DIVIDE_BY_ZERO', 0xC0000374: 'HEAP_CORRUPTION'}
ARCH = {0: 'x86', 5: 'ARM', 9: 'AMD64', 12: 'ARM64', 0xFFFF: '?'}
# ARM64EC: ARM64 register <- AMD64 CONTEXT offset (Rax 0x78 Rcx 0x80 Rdx 0x88 Rbx 0x90 Rsp 0x98 Rbp 0xA0 Rsi 0xA8
# Rdi 0xB0 R8..R15 0xB8..0xF0 Rip 0xF8)
EC_MAP = {0: 0x80, 1: 0x88, 2: 0xB8, 3: 0xC0, 4: 0xC8, 5: 0xD0, 8: 0x78, 19: 0xD8, 20: 0xE0, 21: 0xE8, 22: 0xF0,
          25: 0xA8, 26: 0xB0, 27: 0x90, 29: 0xA0}


def _builtin_classify(w):
    if w & 0x3FFFFC00 == 0x38BFC000:
        return 'rcpc-ldapr'
    if w & 0x3F200C00 == 0x19000000:
        return 'rcpc2-ldapur/stlur'
    if w & 0x3FA07C00 == 0x08A07C00:
        return 'lse-cas'
    if w & 0xBFA07C00 == 0x08207C00:
        return 'lse-casp'
    if w & 0x3F200C00 == 0x38200000:
        return 'lse-ldop/swp'
    if w & 0xFFE0001F == 0xD4200000:
        return 'brk #0x%X' % ((w >> 5) & 0xFFFF)
    return None


try:
    _s = importlib.util.spec_from_file_location('m3scan', M3SCAN)
    _m = importlib.util.module_from_spec(_s)
    _s.loader.exec_module(_m)
    classify_word, CLS = _m.classify, 'm3-scan'
except Exception:                                           # noqa: BLE001 - optional helper
    classify_word, CLS = _builtin_classify, 'builtin'


def wbase(name):
    return name.replace('\\', '/').rsplit('/', 1)[-1]


class Dump:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        if d[:4] != b'MDMP':
            raise SystemExit(f'{path}: not a minidump')
        n, rva = struct.unpack_from('<II', d, 8)
        self.streams = {}
        for i in range(n):
            t, size, off = struct.unpack_from('<III', d, rva + 12 * i)
            self.streams.setdefault(t, (off, size))
        self.mem = []                                       # (start, size, file offset)
        if 9 in self.streams:
            o, _ = self.streams[9]
            cnt, base = struct.unpack_from('<QQ', d, o)
            for i in range(cnt):
                s, sz = struct.unpack_from('<QQ', d, o + 16 + 16 * i)
                self.mem.append((s, sz, base))
                base += sz
        if 5 in self.streams:
            o, _ = self.streams[5]
            cnt, = struct.unpack_from('<I', d, o)
            for i in range(cnt):
                s, sz, r = struct.unpack_from('<QII', d, o + 4 + 16 * i)
                self.mem.append((s, sz, r))
        self.mods = []
        if 4 in self.streams:
            o, _ = self.streams[4]
            cnt, = struct.unpack_from('<I', d, o)
            for i in range(cnt):
                m = o + 4 + 108 * i
                base, size, _, ts, nrva = struct.unpack_from('<QIIII', d, m)
                ln, = struct.unpack_from('<I', d, nrva)
                name = d[nrva + 4:nrva + 4 + ln].decode('utf-16-le')
                self.mods.append((base, size, name, ts))
        self.info = []
        if 16 in self.streams:
            o, _ = self.streams[16]
            hs, es, cnt = struct.unpack_from('<IIQ', d, o)
            for i in range(cnt):
                e = o + hs + es * i
                b, ab, ap, _, rs, st, pr, ty = struct.unpack_from('<QQIIQIII', d, e)
                self.info.append((b, rs, ab, st, pr, ty))
        self.arch = '?'
        if 7 in self.streams:
            self.arch = ARCH.get(struct.unpack_from('<H', d, self.streams[7][0])[0], '?')
        self.threads = []
        if 3 in self.streams:
            o, _ = self.streams[3]
            cnt, = struct.unpack_from('<I', d, o)
            for i in range(cnt):
                t = o + 4 + 48 * i
                tid, = struct.unpack_from('<I', d, t)
                csz, crva = struct.unpack_from('<II', d, t + 40)
                self.threads.append((tid, crva, csz))

    def read(self, addr, n):
        for s, sz, off in self.mem:
            if s <= addr and addr + n <= s + sz:
                return self.d[off + addr - s: off + addr - s + n]
        return None

    def u32(self, a):
        b = self.read(a, 4)
        return None if b is None else struct.unpack('<I', b)[0]

    def u64(self, a):
        b = self.read(a, 8)
        return None if b is None else struct.unpack('<Q', b)[0]

    def module(self, a):
        for base, size, name, _ in self.mods:
            if base <= a < base + size:
                return name, a - base
        return None

    def region(self, a):
        m = self.module(a)
        if m:
            return f'{wbase(m[0])}+0x{m[1]:X}'
        for b, rs, ab, st, pr, ty in self.info:
            if b <= a < b + rs:
                ex = pr & 0xF0 != 0
                kind = {0x1000000: 'image', 0x40000: 'mapped', 0x20000: 'private'}.get(ty, f'type 0x{ty:X}')
                what = ('JIT output (private executable)' if kind == 'private' and ex else
                        'XtaCache-style section (mapped executable)' if kind == 'mapped' and ex else
                        'not executable')
                return f'{kind} region 0x{b:X}+0x{a - b:X} (alloc 0x{ab:X}, protect 0x{pr:X}): {what}'
        return 'not in any listed region'

    def context(self, rva, size):
        """-> dict of named registers (x0..x30, sp, pc, kind)"""
        d = self.d
        flags, = struct.unpack_from('<I', d, rva)
        if size >= 0x110 and flags & 0x00400000:
            r = {f'x{i}': struct.unpack_from('<Q', d, rva + 8 + 8 * i)[0] for i in range(29)}
            r['x29'], r['x30'], r['sp'], r['pc'] = struct.unpack_from('<QQQQ', d, rva + 0xF0)
            r['kind'] = 'ARM64'
            return r
        flags, = struct.unpack_from('<I', d, rva + 0x30)
        if size >= 0x100 and flags & 0x00100000:
            r = {f'x{i}': struct.unpack_from('<Q', d, rva + o)[0] for i, o in EC_MAP.items()}
            r['sp'], = struct.unpack_from('<Q', d, rva + 0x98)
            r['pc'], = struct.unpack_from('<Q', d, rva + 0xF8)
            r['kind'] = 'EC(AMD64 layout)'
            return r
        return {'kind': f'unknown context flags 0x{flags:X} size 0x{size:X}'}


def word_at(dm, a, label):
    w = dm.u32(a)
    if w is None:
        print(f'  {label} 0x{a:X}: {dm.region(a)}; word not in the dump')
        return
    c = classify_word(w) or _builtin_classify(w) or 'ARMv8.0 (no M3-illegal class)'
    print(f'  {label} 0x{a:X}: {dm.region(a)}; word {w:08X} = {c}')


def show_regs(r):
    if 'pc' not in r:
        print('  context:', r['kind'])
        return
    keys = ['pc', 'sp', 'x29', 'x30', 'x19', 'x25', 'x0', 'x1', 'x8']
    print(f'  context {r["kind"]}: ' + ' '.join(f'{k}={r[k]:X}' for k in keys if k in r))


def at_stop(dm, pc):
    m = dm.module(pc)
    if not m:
        return None
    for lo, hi, what in STOPS.get(wbase(m[0]).lower(), []):
        if lo <= m[1] < hi:
            return f'{wbase(m[0])}+0x{m[1]:X} ({what})'
    return None


def main(a):
    if not a:
        raise SystemExit(__doc__)
    dm = Dump(a[0])
    print(f'{a[0]}: arch {dm.arch}, {len(dm.threads)} threads, {len(dm.mods)} modules, '
          f'{len(dm.mem)} memory ranges, {len(dm.info)} info regions; word classifier: {CLS}')
    for base, size, name, ts in dm.mods:
        if any(k in name.lower() for k in ('xtajit', 'sbcpu', 'ntdll', 'kernelbase', 'xtacache', 'fex', 'wow64.dll')):
            print(f'  module {wbase(name)} 0x{base:X} size 0x{size:X} ts 0x{ts:X}')
    exc_tid = None
    if 6 in dm.streams:
        o, _ = dm.streams[6]
        d = dm.d
        exc_tid, = struct.unpack_from('<I', d, o)
        code, flags, rec, addr, npar = struct.unpack_from('<IIQQI', d, o + 8)
        par = struct.unpack_from('<15Q', d, o + 40)[:npar]
        csz, crva = struct.unpack_from('<II', d, o + 160)
        print(f'exception: thread {exc_tid} code {code:08X} {CODES.get(code, "")} flags {flags:X} '
              f'address 0x{addr:X} params ' + ' '.join(f'{p:X}' for p in par))
        word_at(dm, addr, 'exception address')
        show_regs(dm.context(crva, csz))
        if code != 0xC000001D:
            print('  not c000001d: follow the route table in WIN11-M3-X86.md section 1 '
                  '(fail-fast: the real fault is the record x19 points to below)')
    picked = []
    for tid, crva, csz in dm.threads:
        r = dm.context(crva, csz)
        stop = at_stop(dm, r['pc']) if 'pc' in r else None
        if stop or tid == exc_tid or '--all-threads' in a:
            picked.append((0 if stop else 1, tid, r, stop))
    for _, tid, r, stop in sorted(picked, key=lambda p: p[0]):
        print(f'thread {tid}' + (f': stopped at {stop}' if stop else '') + (' (exception thread)' if tid == exc_tid else ''))
        show_regs(r)
        if 'pc' not in r:
            continue
        word_at(dm, r['pc'], 'pc')
        x19, x25, x29 = r.get('x19'), r.get('x25'), r.get('x29')
        if x19:
            code, fl, nxt, ad = dm.u32(x19), dm.u32(x19 + 4), dm.u64(x19 + 8), dm.u64(x19 + 16)
            if code is not None and ad is not None:
                print(f'  x19 EXCEPTION_RECORD 0x{x19:X}: code {code:08X} {CODES.get(code, "")} flags {fl:X} '
                      f'next 0x{nxt or 0:X} address 0x{ad:X}')
                word_at(dm, ad, 'record address')
            else:
                print(f'  x19 0x{x19:X}: {dm.region(x19)}; not in the dump')
        if x25:
            pc = dm.u64(x25 + 0x108)
            if pc is not None:
                print(f'  x25 CONTEXT 0x{x25:X}: flags {dm.u32(x25) or 0:X} Pc(+0x108) 0x{pc:X} '
                      f'Sp 0x{dm.u64(x25 + 0x100) or 0:X} Lr 0x{dm.u64(x25 + 0xF8) or 0:X}')
                word_at(dm, pc, 'CONTEXT.Pc')
            else:
                print(f'  x25 0x{x25:X}: {dm.region(x25)}; not in the dump')
        if x29:
            v = dm.u32(x29 + 0x14)
            print(f'  [x29+0x14] = ' + ('not in the dump' if v is None else f'0x{v:08X}'))
    if not picked:
        print('no thread at an xtajit stop and no exception thread: try --all-threads')


if __name__ == '__main__':
    main(sys.argv[1:])
