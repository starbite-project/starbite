#!/usr/bin/env python3
"""Ventura (13.5) sampler symbolication: shared-cache frames by build/test-boots/dsc135.map (image+offset),
other images by the 'SBMAC[pid WindowServer]: image ADDR PATH' lines.   sample-sym13.py kernel.log [pid]"""
import re, sys, bisect
rows = []
for l in open('build/test-boots/dsc135.map'):
    a, p = l.split(None, 1); rows.append((int(a, 16), p.strip().split('/')[-1]))
rows.sort(); starts = [r[0] for r in rows]
t = open(sys.argv[1], 'rb').read().decode('latin1')
want = sys.argv[2] if len(sys.argv) > 2 else None
imgs = {}
for pid, a, path in re.findall(r'SBMAC\[(\d+) WindowServer\]: image (0x[0-9a-f]+) (\S+)', t):
    imgs.setdefault(pid, []).append((int(a, 16), path.split('/')[-1]))
for v in imgs.values(): v.sort()
blocks = re.split(r'SMP --- sample', t)
last = blocks[-1]
m = re.search(r'SMP WindowServer\[(\d+)\] slide (0x[0-9a-f]+)', last)
pid, slide = m.group(1), int(m.group(2), 16)
if want: pid = want
il = imgs.get(pid, []); ist = [a for a, _ in il]
def sym(a):
    if a >= 0x180000000:
        u = a - slide; i = bisect.bisect_right(starts, u) - 1
        return '%s+%x' % (rows[i][1], u - rows[i][0])
    i = bisect.bisect_right(ist, a) - 1
    return '%s+%x' % (il[i][1], a - il[i][0]) if i >= 0 else hex(a)
print('pid', pid, 'slide', hex(slide))
for line in last.splitlines():
    m = re.search(r'SMP WindowServer (t\d+) pc (0x[0-9a-f]+) lr (0x[0-9a-f]+) sp \S+ bt ([0-9a-fx ]*)', line)
    if m:
        print(m[1], ' <- '.join(sym(int(x, 16)) for x in [m[2], m[3]] + m[4].split()))
