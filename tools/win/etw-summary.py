#!/usr/bin/env python3
"""etw-summary.py CSV... [--pid N] [--rows N]: summary of a tracerpt CSV from VIS2/VIS3 (docs/WIN11-GPU.md 6.85).

tracerpt -of CSV writes one header block, then one line per event: Event Name, Type (opcode name), Event ID, Version,
Channel, Level, Opcode, Task, Keyword, PID, TID, Processor Number, Instance ID, Parent Instance ID, Activity ID,
Related Activity ID, Clock-Time, Kernel(ms), User(ms), User Data, then the event's own fields (unnamed, comma
separated). This prints, per file:
  - the processes seen (PID -> event count; sbcomp's and dwm's PIDs are the ones that matter);
  - event counts by provider/task + opcode name, overall and for the busiest app PID;
  - the first --rows events (default 25) of the app PID whose name mentions Present, Blt, Flip, Redirect, Token,
    Composition, Surface or Error, with their payload, so the path a present takes is visible in order.
Read-only; no Windows needed.
"""
import collections
import csv
import re
import sys

KEYS = re.compile(r'present|blt|blit|flip|redirect|token|composition|surface|error|fail|swapchain', re.I)


def rows(path):
    with open(path, encoding='utf-8-sig', errors='replace', newline='') as f:
        r = csv.reader(f, skipinitialspace=True)
        header = None
        for row in r:
            if not row:
                continue
            if header is None:
                if row[0].strip().lower() == 'event name':
                    header = [h.strip() for h in row]
                continue
            yield header, [c.strip() for c in row]


def main(a):
    pid_want = None
    nrows = 25
    files = []
    i = 0
    while i < len(a):
        if a[i] == '--pid':
            pid_want = a[i + 1]; i += 2
        elif a[i] == '--rows':
            nrows = int(a[i + 1]); i += 2
        else:
            files.append(a[i]); i += 1
    if not files:
        raise SystemExit(__doc__)
    for path in files:
        print(f'=== {path}')
        evs = []
        for header, row in rows(path):
            def col(name, default=''):
                try:
                    return row[header.index(name)]
                except (ValueError, IndexError):
                    return default
            name, typ, task, pid = col('Event Name'), col('Type'), col('Task'), col('PID')
            n = len(header)
            payload = row[n - 1:] if len(row) >= n else []
            evs.append((name, typ, task, pid, col('Clock-Time'), payload))
        if not evs:
            print('  no events (header not found?)')
            continue
        pids = collections.Counter(e[3] for e in evs)
        print('  events:', len(evs), ' PIDs (count):', ', '.join(f'{p}:{c}' for p, c in pids.most_common(8)))
        kinds = collections.Counter(f'{e[0]}/{e[2]} {e[1]}' for e in evs)
        print('  by event/task + opcode (top 40):')
        for k, c in kinds.most_common(40):
            print(f'    {c:7d}  {k}')
        target = pid_want
        if target is None:
            # the busiest PID that is not 0/4 (System) - usually dwm; list the top three so sbcomp's is visible
            cands = [p for p, _ in pids.most_common() if p not in ('0', '4', '0x0', '0x4')]
            target = cands[0] if cands else None
            print('  busiest app PIDs:', ', '.join(cands[:4]))
        if target is None:
            continue
        sel = [e for e in evs if e[3] == target and KEYS.search(f'{e[0]} {e[1]} {e[2]}')]
        print(f'  PID {target}: {len(sel)} present-related events; first {min(nrows, len(sel))}:')
        for e in sel[:nrows]:
            print(f'    {e[4]}  {e[0]}/{e[2]} {e[1]}  ' + ' | '.join(x for x in e[5] if x)[:240])
        tk = collections.Counter(f'{e[0]}/{e[2]} {e[1]}' for e in evs if e[3] == target)
        print(f'  PID {target} by kind:', '; '.join(f'{k} {c}' for k, c in tk.most_common(15)))


if __name__ == '__main__':
    main(sys.argv[1:])
