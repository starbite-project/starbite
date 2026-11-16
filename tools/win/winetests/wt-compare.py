#!/usr/bin/env python3
"""wt-compare.py LABEL=LOG [LABEL=LOG ...] [--all]: sbwt conformance logs side by side.

Each LOG is an sbwt log (umdlog/sbwt-<suite>[-A|-B].log). Prints the summary per log, then one row per test whose
result differs between the logs or is not PASS in any of them (--all: every test), with failures/executed counts,
and the totals of tests that changed class between the first log and each other one. Read-only.
"""
import re
import sys

LINE = re.compile(r'^SBWT (\S+) (\S+): (PASS|FAIL|CRASH|TIMEOUT|NOSTART); exit (0x[0-9a-f]+); executed (\d+), failures (\d+)')


def load(path):
    res = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = LINE.match(line.replace('\r', ''))
        if m:
            res[m.group(2)] = (m.group(3), m.group(4), int(m.group(5)), int(m.group(6)))
    # T76b: a run where the device fell back to the Basic Render Driver scored 110/111 and meant nothing. Newer sbwt
    # logs say so per line; for older ones look at the per-test outputs next to the log.
    import glob, os
    foreign = 0
    for f in glob.glob(os.path.join(path + '.out', '*.txt')):
        for line in open(f, encoding='utf-8', errors='replace'):
            if 'Adapter: L"' in line:
                foreign += 'Starbite' not in line
                break
    if foreign:
        print(f'!! {os.path.basename(path)}: {foreign} tests ran on ANOTHER ADAPTER (not our driver): this log is not a driver score')
    return res


def cell(r):
    if r is None:
        return '-'
    cls, code, ex, fl = r
    if cls == 'PASS':
        return f'pass {ex}'
    if cls == 'FAIL':
        return f'FAIL {fl}/{ex}'
    if cls == 'CRASH':
        return f'CRASH {code[-8:]}'
    return cls


def main(a):
    show_all = '--all' in a
    pairs = [x.split('=', 1) for x in a if '=' in x]
    if not pairs:
        raise SystemExit(__doc__)
    logs = [(lab, load(p)) for lab, p in pairs]
    names = []
    for _, r in logs:
        for n in r:
            if n not in names:
                names.append(n)
    for lab, r in logs:
        c = {}
        for v in r.values():
            c[v[0]] = c.get(v[0], 0) + 1
        print(f'{lab:10s} {len(r):4d} tests: ' + ', '.join(f'{k.lower()} {c[k]}' for k in ('PASS', 'FAIL', 'CRASH', 'TIMEOUT', 'NOSTART') if k in c))
    w = max(len(n) for n in names) if names else 10
    print(f'{"test":{w}s}  ' + '  '.join(f'{lab:>16s}' for lab, _ in logs))
    for n in names:
        rs = [r.get(n) for _, r in logs]
        classes = {x[0] if x else None for x in rs}
        if show_all or classes != {'PASS'}:
            print(f'{n:{w}s}  ' + '  '.join(f'{cell(x):>16s}' for x in rs))
    base = logs[0][1]
    for lab, r in logs[1:]:
        better = [n for n in names if n in base and n in r and base[n][0] != 'PASS' and r[n][0] == 'PASS']
        worse = [n for n in names if n in base and n in r and base[n][0] == 'PASS' and r[n][0] != 'PASS']
        print(f'{logs[0][0]} -> {lab}: {len(better)} now pass ({", ".join(better)}); {len(worse)} no longer pass ({", ".join(worse)})')


if __name__ == '__main__':
    main(sys.argv[1:])
