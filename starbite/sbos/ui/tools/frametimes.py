#!/usr/bin/env python3
# frametimes.py FRAMES.txt [HOSTBENCH.txt UIBENCH.txt]: the per-frame host times of an sbtk-host film ("F" lines)
# per kind of frame (half = a motion frame: half-resolution compose + the 2x present; full = at rest), and, given
# the host's and the phone's uibench lines, the A55 estimate per uibench set: each frame's compose time scaled by
# host / A55 throughput of "blend, a half-resolution layer", its present by "2x upscale ... into the panel's
# framebuffer" (A55) against the host's 2x into RAM. The host compositor runs on one core: a set with 4 cores
# assumes the kernel splits the rows over them as uibench does.
import re, sys, statistics as st
rows = [l.split() for l in open(sys.argv[1]) if l.startswith('F ')]
def col(r, k): return float(r[r.index(k) + 1])
kinds = {}
for r in rows:
    k = r[3] if col(r, 'px') else 'idle'
    kinds.setdefault(k, []).append(r)
def mbs(lines, key):
    for l in lines:
        if key in l:
            m = re.search(r'([0-9]+) MB/s', l)
            if m: return float(m.group(1))
    return None
est = []
if len(sys.argv) > 3:
    host = open(sys.argv[2]).read().splitlines()
    hb, hu = mbs(host, 'blend, a half-resolution'), mbs(host, '2x upscale, half frame to a full frame in RAM')
    sets, cur = {}, None
    for l in open(sys.argv[3]):
        m = re.search(r'uibench: set: (.*)', l)
        if m: cur = m.group(1).strip(); sets[cur] = []; continue
        if cur and 'uibench:' in l: sets[cur].append(l)
    for name, ls in sets.items():
        ab, au = mbs(ls, 'blend, a half-resolution'), mbs(ls, "2x upscale, half frame into the panel")
        if ab and au: est.append((name, hb / ab, hu / au))
print(f'{sys.argv[1]}: {len(rows)} frames at 60 fps')
for k in ('half', 'full'):
    rs = kinds.get(k, [])
    if not rs: continue
    t = sorted(col(r, 'total') for r in rs)
    c = [col(r, 'comp') for r in rs]; p = [col(r, 'present') for r in rs]; px = [col(r, 'px') for r in rs]
    p90 = t[int(.9 * (len(t) - 1))]
    print(f'  {k:5s} n {len(rs):4d}  host mean {st.mean(t):5.2f} p90 {p90:5.2f} max {t[-1]:5.2f} ms  (compose {st.mean(c):.2f} + present {st.mean(p):.2f})  {st.mean(px)/1e6:.2f} M panel px')
    for name, rb, ru in est:
        e = sorted(col(r, 'comp') * rb + col(r, 'present') * ru for r in rs)
        fits = sum(1 for v in e if v <= 16.7) / len(e)
        print(f'        A55 [{name}]: mean {st.mean(e):6.1f} p90 {e[int(.9 * (len(e) - 1))]:6.1f} max {e[-1]:6.1f} ms; {100 * fits:.0f} % of frames within 16.7 ms (x{rb:.1f} compose, x{ru:.1f} present)')
print(f'  idle frames (nothing changed): {len(kinds.get("idle", []))}')
