#!/usr/bin/env python3
# frametimes.py FRAMES.txt [A55.json]: per-frame host times of an sbtk-host film ("F" lines), summarised per kind of
# frame (half = a motion frame, full = at rest), and the A55 estimate when uibench's numbers are given.
import sys, statistics as st
rows = [l.split() for l in open(sys.argv[1]) if l.startswith('F ')]
def col(r, k): return float(r[r.index(k) + 1])
kinds = {}
for r in rows:
    k = r[3]
    if col(r, 'px') == 0: k += '-idle'
    kinds.setdefault(k, []).append(r)
print(f'{sys.argv[1]}: {len(rows)} frames')
for k, rs in sorted(kinds.items()):
    t = sorted(col(r, 'total') for r in rs)
    c = [col(r, 'comp') for r in rs]; p = [col(r, 'present') for r in rs]; px = [col(r, 'px') for r in rs]
    p90 = t[int(.9 * (len(t) - 1))]
    print(f'  {k:10s} n {len(rs):4d}  total mean {st.mean(t):6.2f} p90 {p90:6.2f} max {t[-1]:6.2f} ms  (compose {st.mean(c):.2f}, present {st.mean(p):.2f})  panel px {st.mean(px)/1e6:.2f} M')
