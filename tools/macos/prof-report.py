#!/usr/bin/env python3
"""prof-report.py: where a process spends its CPU, from the kernel's "SWEET PROF" lines (xnu pending 0458,
boot-arg sweetprof=NAME; 500 samples a second on every core, printed every 10 s while the process is busy).

  tools/macos/prof-report.py KERNEL_LOG [--kernel out/kernel.development.starlte-12377-seqXX]
        [--a2s /Volumes/SBWORK/claude-scratch-apple/a2s.cache] [--image NAME=PATH ...] [--windows] [--top N]

Shared-cache addresses are named from the a2s cache (address -> symbol table of the macOS 15 shared cache), kernel
addresses with nm on --kernel, an image outside the cache with nm on --image (NAME = the last path component in
its "SWEET PROF img" line). Without a table an address is shown as image+offset.
Prints: totals, the busiest threads, functions by their own samples, and function <- caller pairs (the caller
is the function the link register points into: exact for leaf functions, the last callee's return otherwise).
"""
import argparse, bisect, collections, mmap, os, re, struct, subprocess, sys


class A2S:
    def __init__(self, path):
        self.fh = open(path, 'rb')
        self.m = mmap.mmap(self.fh.fileno(), 0, access=mmap.ACCESS_READ)
        self.cnt = struct.unpack_from('<I', self.m, 4)[0]
        self.strtab = 0x18 + 12 * self.cnt - 12

    def sym(self, x):
        m, lo, hi = self.m, 0, self.cnt - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if struct.unpack_from('<Q', m, 0x18 + 12 * mid)[0] <= x:
                lo = mid
            else:
                hi = mid - 1
        addr, off = struct.unpack_from('<QI', m, 0x18 + 12 * lo)
        if addr > x or x - addr > 0x40000:
            return None
        e = m.find(b'\0', self.strtab + off)
        return m[self.strtab + off:e].decode(errors='replace')


class NmTable:
    """Symbols of a Mach-O file by address (nm -n); base = the address its first byte is linked at."""
    def __init__(self, path):
        out = subprocess.run(['nm', '-n', path], capture_output=True, text=True).stdout
        self.addrs, self.names = [], []
        for l in out.splitlines():
            p = l.split()
            if len(p) >= 3 and p[1] in 'TtSs' and re.fullmatch(r'[0-9a-f]+', p[0]):
                self.addrs.append(int(p[0], 16))
                self.names.append(p[2])
        self.base = 0
        seg = subprocess.run(['otool', '-l', path], capture_output=True, text=True).stdout
        m = re.search(r'segname __TEXT\n\s+vmaddr (0x[0-9a-f]+)', seg)
        if m:
            self.base = int(m.group(1), 16)

    def sym(self, x):
        i = bisect.bisect_right(self.addrs, x) - 1
        return self.names[i] if i >= 0 and x - self.addrs[i] < 0x100000 else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('--kernel')
    ap.add_argument('--a2s', default='/Volumes/SBWORK/claude-scratch-apple/a2s.cache')
    ap.add_argument('--image', action='append', default=[])
    ap.add_argument('--windows', action='store_true', help='one summary line per 10 s window too')
    ap.add_argument('--top', type=int, default=45)
    a = ap.parse_args()

    a2s = A2S(a.a2s) if os.path.exists(a.a2s) else None
    ktab = NmTable(a.kernel) if a.kernel else None
    itabs = {}
    for spec in a.image:
        name, path = spec.split('=', 1)
        itabs[name] = NmTable(path)

    imgs = {}                       # load address -> path
    slide = mh = 0
    user = collections.Counter()    # (pc, lr) -> samples
    kern = collections.Counter()
    threads = collections.Counter()
    tot = collections.Counter()
    windows = []
    head = re.compile(r'SWEET PROF (\S+)\[(\d+)\]: (\d+) samples \(user (\d+) kernel (\d+); other (\d+) idle (\d+); dropped (\d+)\) '
                      r'slide 0x([0-9a-f]+) mh 0x([0-9a-f]+) kslide 0x([0-9a-f]+)')
    for line in open(a.log, errors='replace'):
        i = line.find('SWEET PROF ')
        if i < 0:
            continue
        ts = re.match(r'\[\s*([0-9.]+)\]', line)
        l = line[i:].rstrip('\n')
        m = head.match(l)
        if m:
            tot['samples'] += int(m.group(3)); tot['user'] += int(m.group(4)); tot['kernel'] += int(m.group(5))
            tot['other'] += int(m.group(6)); tot['idle'] += int(m.group(7)); tot['dropped'] += int(m.group(8))
            tot['windows'] += 1
            slide, mh = int(m.group(9), 16), int(m.group(10), 16)
            windows.append((ts.group(1) if ts else '?', int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7))))
            who = f'{m.group(1)}[{m.group(2)}]'
            continue
        if l.startswith('SWEET PROF img '):
            p = l.split(None, 4)
            if len(p) >= 5:
                imgs[int(p[3], 16)] = p[4]
            continue
        m = re.match(r'SWEET PROF ([tuk]):(.*)', l)
        if not m:
            continue
        for item in m.group(2).split():
            f = item.split(':')
            if m.group(1) == 't' and len(f) >= 3:
                threads[(f[0], f[2])] += int(f[1])
            elif len(f) == 3:
                (user if m.group(1) == 'u' else kern)[(int(f[0], 16), int(f[1], 16))] += int(f[2])
    if not tot['windows']:
        print('no "SWEET PROF name[pid]: N samples" lines in', a.log)
        return 1
    if mh and mh not in imgs:
        imgs[mh] = '(main binary)'
    loads = sorted(imgs)

    def uname(x):
        if x >= 0x180000000 + slide:
            s = a2s.sym(x - slide) if a2s else None
            return s or f'cache+{x - slide - 0x180000000:#x}'
        i = bisect.bisect_right(loads, x) - 1
        if i < 0:
            return f'{x:#x}'
        base = loads[i]
        short = imgs[base].rstrip('/').split('/')[-1]
        t = itabs.get(short)
        s = t.sym(x - base + t.base) if t else None
        return f'{short}`{s}' if s else f'{short}+{x - base:#x}'

    def kname(x):
        s = ktab.sym(x) if ktab else None
        return 'kernel`' + (s or f'{x:#x}')

    n = tot['samples']
    cores = tot['samples'] + tot['other'] + tot['idle']
    print(f'{who}: {n} samples in {tot["windows"]} busy windows of 10 s = {n / 500:.1f} s of CPU '
          f'({100 * tot["user"] / n:.0f} % user, {100 * tot["kernel"] / n:.0f} % kernel); other processes {tot["other"]}, '
          f'idle {tot["idle"]}, dropped {tot["dropped"]}')
    if cores:
        print(f'  share of all sampled core time: {100 * n / cores:.0f} % (other {100 * tot["other"] / cores:.0f} %, idle {100 * tot["idle"] / cores:.0f} %)')
    if a.windows:
        for t, u, k, o, i in windows:
            print(f'  window ending {t} s: user {u} kernel {k} = {100 * (u + k) / 5000:.0f} % of one core; other {o}, idle {i}')
    print('\nthreads:')
    for (tid, name), c in threads.most_common(8):
        print(f'  {100 * c / n:5.1f} %  {tid} {name}')
    listed = sum(user.values()) + sum(kern.values())
    print(f'\nfunctions by own samples ({100 * listed / n:.0f} % of the samples are in the listed buckets):')
    fn = collections.Counter()
    pair = collections.Counter()
    img = collections.Counter()
    # an address names a 16-byte bucket: its middle is looked up (a function may begin inside the bucket)
    for (pc, lr), c in user.items():
        f = uname(pc + 8)
        fn[f] += c
        pair[(f, uname(lr + 8))] += c
        img[f.split('`')[0].split('+')[0] if '`' in f or '+' in f else 'shared cache'] += c
    for (pc, lr), c in kern.items():
        f = kname(pc + 8)
        fn[f] += c
        pair[(f, kname(lr + 8))] += c
        img['kernel'] += c
    for f, c in fn.most_common(a.top):
        print(f'  {100 * c / n:5.1f} %  {c:6d}  {f}')
    print('\nfunction <- link register:')
    for (f, g), c in pair.most_common(a.top):
        print(f'  {100 * c / n:5.1f} %  {c:6d}  {f}  <-  {g}')
    print('\nby image:')
    for f, c in img.most_common(12):
        print(f'  {100 * c / n:5.1f} %  {f}')
    print('\nimages outside the shared cache:')
    for b in loads:
        print(f'  {b:#x} {imgs[b]}')
    return 0


sys.exit(main())
