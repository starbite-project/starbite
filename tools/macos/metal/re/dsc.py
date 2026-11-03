#!/usr/bin/env python3
"""Low-memory dyld shared cache helper (reads only headers, the .map and the bytes asked for).

  dsc.py stub  CACHE 0xADDR [..]   -> what a branch-island / stub at ADDR jumps to, and the image
  dsc.py image CACHE 0xADDR [..]   -> which image (and segment) holds ADDR (from CACHE.map)
  dsc.py calls CACHE DYLIB 0xSTART 0xEND
                                   -> every BL in [START, END) of an extracted DYLIB, with the stub
                                      resolved to image + symbol (images are extracted on demand
                                      with `ipsw dsc extract` into $DSC_EXTRACT, default ./dsc27)

CACHE is the main file (dyld_shared_cache_arm64e); its subcaches are found next to it.
(`ipsw dsc extract --stubs --objc` needed 8 GB of RAM on macOS 27's cache; this needs ~100 MB.)
"""
import bisect
import glob
import mmap
import os
import re
import struct
import subprocess
import sys

IPSW = os.environ.get('IPSW', os.path.join(os.path.dirname(__file__), '../../../../build/tools/ipsw/ipsw'))


class Cache:
    def __init__(self, main):
        self.main = main
        self.maps = []          # (addr, size, fileoff, mmap)
        files = [main] + sorted(f for f in glob.glob(main + '.*')
                                if re.fullmatch(r'\.\d+(\.dyld\w+)?', f[len(main):]))
        for f in files:
            fd = open(f, 'rb')
            m = mmap.mmap(fd.fileno(), 0, access=mmap.ACCESS_READ)
            if m[:4] != b'dyld':
                continue
            moff, mcount = struct.unpack_from('<II', m, 0x10)
            for i in range(mcount):
                addr, size, foff, _, _ = struct.unpack_from('<QQQII', m, moff + i * 32)
                self.maps.append((addr, size, foff, m))
        self.maps.sort(key=lambda x: x[0])
        self.images = None

    def ptr(self, raw):
        # slide info v5 (arm64e caches since macOS 14): the low 34 bits are the target's offset
        # from the cache base, whatever the auth/high8 bits say
        return self.maps[0][0] + (struct.unpack('<Q', raw)[0] & 0x3ffffffff)

    def read(self, addr, n):
        for a, size, foff, m in self.maps:
            if a <= addr < a + size:
                return m[foff + addr - a:foff + addr - a + n]
        return None

    def load_map(self):
        if self.images is not None:
            return
        segs = []
        cur = None
        with open(self.main + '.map', 'r', errors='replace') as f:
            for line in f:
                if line.startswith('/'):
                    cur = line.strip()
                    continue
                mm = re.match(r'\s*(__\w+)\s+0x([0-9A-Fa-f]+) -> 0x([0-9A-Fa-f]+)', line)
                if mm and cur:
                    segs.append((int(mm.group(2), 16), int(mm.group(3), 16), mm.group(1), cur))
        segs.sort()
        self.images = segs
        self.starts = [s[0] for s in segs]

    def image_of(self, addr):
        self.load_map()
        i = bisect.bisect_right(self.starts, addr) - 1
        if i >= 0 and self.images[i][0] <= addr < self.images[i][1]:
            return self.images[i]
        return None

    def stub_target(self, addr, depth=0):
        """Follow adrp/add/br, adrp/ldr/br(aa) and b chains to the final code address."""
        b = self.read(addr, 16)
        if b is None:
            return None, 'unmapped'
        w = struct.unpack('<4I', b)
        if (w[0] & 0xfc000000) == 0x14000000:                      # b imm26
            imm = w[0] & 0x3ffffff
            if imm & (1 << 25):
                imm -= 1 << 26
            t = addr + imm * 4
            return (self.stub_target(t, depth + 1) if depth < 4 else (t, 'b'))
        if (w[0] & 0x9f000000) == 0x90000000:                      # adrp
            imm = ((w[0] >> 29) & 3) | (((w[0] >> 5) & 0x7ffff) << 2)
            if imm & (1 << 20):
                imm -= 1 << 21
            page = (addr & ~0xfff) + (imm << 12)
            if (w[1] & 0xffc00000) == 0x91000000:                  # add
                t = page + ((w[1] >> 10) & 0xfff)
                if (w[2] & 0xfffffc1f) == 0xd61f0000:              # br xN
                    return t, 'direct'
                if (w[2] & 0xffc00000) == 0xf9400000:              # ldr x16, [x17] ; braa
                    p = self.read(t + ((w[2] >> 10) & 0xfff) * 8, 8)
                    return self.ptr(p), 'auth-got %#x' % t
                return t, 'adrp+add'
            if (w[1] & 0xffc00000) == 0xf9400000:                  # ldr x16, [x16, #off]
                ptr = page + ((w[1] >> 10) & 0xfff) * 8
                p = self.read(ptr, 8)
                return self.ptr(p), 'got %#x' % ptr
        return addr, 'code'


_symcache = {}


def symbols_of(image_path, outdir):
    name = os.path.basename(image_path)
    if name in _symcache:
        return _symcache[name]
    path = os.path.join(outdir, name)
    if not os.path.exists(path):
        # never --stubs/--objc (8 GB of RAM, and a 1.7 GB .a2s in $TMPDIR); ipsw's temp files go to the
        # external drive, not the Mac's disk
        env = dict(os.environ, TMPDIR=os.environ.get('DSC_TMP', '/Volumes/Untitled/starbite-metal/tmp'))
        os.makedirs(env['TMPDIR'], exist_ok=True)
        subprocess.run([IPSW, 'dsc', 'extract', CACHE_PATH, image_path, '-o', outdir, '--slide'],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    syms = []
    if os.path.exists(path):
        out = subprocess.run(['nm', '-n', path], capture_output=True, text=True).stdout
        for line in out.splitlines():
            mm = re.match(r'([0-9a-f]{8,16}) [tT] (.*)', line)
            if mm:
                syms.append((int(mm.group(1), 16), mm.group(2)))
    _symcache[name] = (syms, [s[0] for s in syms])
    return _symcache[name]


def name_of(cache, addr, outdir):
    img = cache.image_of(addr)
    if not img:
        return '?'
    syms, keys = symbols_of(img[3], outdir)
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0:
        return '%s:%#x' % (os.path.basename(img[3]), addr)
    off = addr - syms[i][0]
    return '%s`%s%s' % (os.path.basename(img[3]), syms[i][1], '+%#x' % off if off else '')


def cstr(cache, a, n=120):
    b = cache.read(a, n)
    if not b:
        return None
    e = b.find(b'\0')
    if e <= 0:
        return None
    try:
        t = b[:e].decode()
    except UnicodeDecodeError:
        return None
    return t if t.isprintable() else None


def describe(cache, t):
    if t is None:
        return ''
    s = cstr(cache, t)
    if s and len(s) >= 2:
        return '"%s"' % s
    raw = cache.read(t, 32)
    if raw:
        # CFString: isa, flags(0x7c8 / 0x7d0), cstring pointer, length
        isa, flags, p, n = struct.unpack('<QQQQ', raw)
        if flags & 0xffffffff in (0x7c8, 0x7d0) and n < 4096:
            s = cstr(cache, cache.ptr(raw[16:24]))
            if s is not None:
                return '@"%s"' % s
        # selector reference / pointer to a string
        s = cstr(cache, cache.ptr(raw[:8]))
        if s and len(s) >= 3:
            return '*"%s"' % s
    return ''


def main():
    global CACHE_PATH
    cmd, CACHE_PATH = sys.argv[1], sys.argv[2]
    c = Cache(CACHE_PATH)
    outdir = os.environ.get('DSC_EXTRACT', 'dsc27')
    if cmd == 'image':
        for a in sys.argv[3:]:
            print(a, c.image_of(int(a, 16)))
    elif cmd == 'stub':
        for a in sys.argv[3:]:
            t, how = c.stub_target(int(a, 16))
            print('%s -> %#x (%s) %s' % (a, t, how, name_of(c, t, outdir)))
    elif cmd == 'dis':
        # annotated disassembly: bl/b targets resolved, adrp+add/ldr targets shown as C strings,
        # CFStrings (their C string) or selector references
        dylib, start, end = sys.argv[3], int(sys.argv[4], 16), int(sys.argv[5], 16)
        out = subprocess.run(['objdump', '--no-show-raw-insn', '-d', '--start-address=%#x' % start,
                              '--stop-address=%#x' % end, dylib], capture_output=True, text=True).stdout
        pages = {}
        for line in out.splitlines():
            mm = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s+(.*)', line)
            if not mm:
                continue
            pc, op, args = int(mm.group(1), 16), mm.group(2), mm.group(3)
            note = ''
            if op in ('bl', 'b') and args.startswith('0x'):
                t = int(args.split()[0], 16)
                if not (start <= t < end):
                    real, how = c.stub_target(t)
                    note = name_of(c, real, outdir)
            elif op == 'adrp':
                r = args.split(',')[0]
                pages[r] = int(args.split(',')[1].split()[0], 16)
            elif op in ('add', 'ldr') and '#' in args:
                regs = [x.strip(' [') for x in args.split(',')]
                base = regs[1]
                if pages.get(base) is not None:
                    imm = int(re.search(r'#(0x[0-9a-f]+|\d+)', args).group(1), 0)
                    t = pages[base] + imm
                    note = describe(c, t)
                    if op == 'add':
                        pages[regs[0]] = None
            print('%x: %-6s %-40s %s' % (pc, op, args[:40], note))
    elif cmd == 'calls':
        dylib, start, end = sys.argv[3], int(sys.argv[4], 16), int(sys.argv[5], 16)
        out = subprocess.run(['objdump', '--no-show-raw-insn', '-d', '--start-address=%#x' % start,
                              '--stop-address=%#x' % end, dylib], capture_output=True, text=True).stdout
        for line in out.splitlines():
            mm = re.match(r'\s*([0-9a-f]+):\s+(bl|b)\s+0x([0-9a-f]+)', line)
            if mm:
                t = int(mm.group(3), 16)
                real, how = c.stub_target(t)
                print('%s %s %s' % (mm.group(1), mm.group(2), name_of(c, real, outdir)))


if __name__ == '__main__':
    main()
