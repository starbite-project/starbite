#!/usr/bin/env python3
"""Summarize an iOS 26 test boot's kernel log (build/test-boots/<time>-KEY/kernel.log, sdlog=1).
    tools/ios26-log-summary.py build/test-boots/<dir>
Prints: the card bring-up and speed lines (kernel 0400/0401), memory, frame rate, crashes by process
with their reasons, exits, overlay lines, the first and last timestamps."""
import collections
import re
import sys
from pathlib import Path


def main():
    d = Path(sys.argv[1])
    f = d / 'kernel.log' if d.is_dir() else d
    text = f.read_bytes().decode('utf-8', 'replace')
    lines = [l for l in text.splitlines() if l.strip()]
    ts = [float(m.group(1)) for l in lines if (m := re.match(r'\[\s*([0-9.]+)\]', l))]
    print(f'{f}: {len(lines)} lines, {ts[0] if ts else "?"} .. {ts[-1] if ts else "?"} s')
    pick = ('STARBITE SD:', 'STARLTE OVERLAY', 'STARBITE HEALTH', 'HEROLTE LOG: sweetreboot', 'cannot mount root',
            'panic')
    print('\n== card / overlay / health')
    for l in lines:
        if any(p in l for p in pick) and 'io 60s' not in l:
            print(' ', l[:220])
    print('\n== io per minute')
    for l in lines:
        if 'io 60s' in l:
            print(' ', l[:240])
    print('\n== memory (every 4th STARLTE MEM)')
    mem = [l for l in lines if 'STARLTE MEM' in l]
    for l in mem[::4] + mem[-1:]:
        print(' ', l[:240])
    print('\n== frames (every 6th)')
    fb = [l for l in lines if 'A5XELTE FB: frames' in l]
    for l in fb[::6] + fb[-1:]:
        print(' ', l[:160])
    print('\n== crashes')
    crashes = collections.Counter()
    reasons = {}
    for l in lines:
        m = re.search(r'CRASH (\S+?)\[\d+\] sig (\d+)', l)
        if m:
            crashes[(m.group(1), m.group(2))] += 1
        m = re.search(r'CRASH (\S+) reason (.*)', l)
        if m:
            reasons.setdefault(m.group(1), m.group(2)[:200])
    for (name, sig), n in crashes.most_common():
        print(f'  {n:3}x {name} sig {sig}: {reasons.get(name, "")}')
    print('\n== exits / jetsam / kills')
    ex = collections.Counter()
    for l in lines:
        if 'STARBITE EXIT' in l or 'jetsam' in l.lower() or 'memorystatus_kill' in l or 'killed' in l:
            ex[re.sub(r'\[\s*[0-9.]+\]: ', '', re.sub(r'\[\d+\]', '', l))[:160]] += 1
    for l, n in ex.most_common(25):
        print(f'  {n:3}x {l}')


if __name__ == '__main__':
    main()
