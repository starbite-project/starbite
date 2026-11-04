#!/usr/bin/env python3
"""Symbolicate the kernel sampler's (sweetsample) running WindowServer threads: shared-cache frames
by build/test-boots/dsc265.map, our StarbiteMetal / libvulkan_panfrost frames by atos on the
unstripped builds, with the load addresses sbmac logs.   sample-sym.py kernel.log"""
import re, subprocess, sys, collections
t = open(sys.argv[1], 'rb').read().decode('latin1')
SYM = {'StarbiteMetal': '/Volumes/sbmtl-build/out/sym/StarbiteMetal',
       'libvulkan_panfrost.dylib': '/Volumes/sbmtl-build/out/sym/libvulkan_panfrost.dylib'}
loads = collections.defaultdict(dict)
for pid, a, path in re.findall(r'SBMAC\[(\d+) WindowServer\]: image (0x[0-9a-f]+) (\S+)', t):
    name = path.split('/')[-1]
    if name in SYM:
        loads[pid][name] = int(a, 16)
rows, name = [], None
for l in open('build/test-boots/dsc265.map'):
    if l.startswith('/'): name = l.strip()
    m = re.search(r'__TEXT (0x[0-9A-F]+) -> (0x[0-9A-F]+)', l)
    if m: rows.append((int(m[1], 16), int(m[2], 16), name))
def ours(pid, a):
    best = None
    for n, base in loads.get(pid, {}).items():
        if base <= a and (best is None or base > best[1]): best = (n, base)
    return best
cur = None
for line in t.splitlines():
    m = re.search(r'SMP WindowServer\[(\d+)\] slide (0x[0-9a-f]+)', line)
    if m: cur = (m.group(1), int(m.group(2), 16)); print('== sample', line[:16], 'pid', cur[0]); continue
    m = re.search(r'SMP WindowServer (t\d+) pc (0x[0-9a-f]+) lr (0x[0-9a-f]+) sp \S+ bt ([0-9a-fx ]+)', line)
    if not m or not cur: continue
    pcs = [int(m.group(2), 16), int(m.group(3), 16)] + [int(x, 16) for x in m.group(4).split()]
    if not any(0x100000000 <= a < 0x180000000 for a in pcs[:3]):
        continue   # only threads whose pc is in our (non-cache) code
    print(' ', m.group(1))
    for a in pcs:
        if a < 0x100000000: continue
        o = ours(cur[0], a) if a < 0x180000000 else None
        if o:
            s = subprocess.run(['atos', '-o', SYM[o[0]], '-l', hex(o[1]), hex(a)], capture_output=True, text=True).stdout.strip()
            print('    ', hex(a), s)
        else:
            x = a - cur[1]
            print('    ', hex(a), next((f'{n.split("/")[-1]}+{x - s:#x}' for s, e, n in rows if s <= x < e), '?'))
