#!/usr/bin/env python3
"""user-report.py DIR [--since ISO]: summary of a user-session pull (tools/win/user-pull.sh; docs/WIN11-GPU.md 6.70).

DIR: umdlog/, SYSTEM, evtx/*.evtx. Prints GD frame times per log (glstats fps, worst intervals; gllows/glworst lines
from 0158 when present), sbwddm/sbwdog/sbthermal state, boots and shutdowns (System 12/13/6005/6006/6008/1074/41,
Diagnostics-Performance 100/200 with durations and 101-110/201-207: what slowed them), service trouble (7000-7043),
and app crashes/hangs (Application 1000/1002) since --since (default: the last 12 h in the log)."""
import os, re, sys, glob, subprocess, datetime as dt
from collections import Counter

d = sys.argv[1]
since = None
if '--since' in sys.argv:
    since = sys.argv[sys.argv.index('--since') + 1]
here = os.path.dirname(os.path.abspath(__file__))
hive_py = os.path.join(here, '..', 'woa', 'hive.py')


def head(t):
    print('\n== ' + t)


# ---- GD frame times ----
head('Geometry Dash frame times (umdlog)')
for f in sorted(glob.glob(os.path.join(d, 'umdlog', 'GeometryDash.exe-*.log')), key=os.path.getmtime):
    txt = open(f, errors='replace').read()
    rows = re.findall(r'glstats ([0-9.]+) s: (\d+) frames \(([0-9.]+) fps.*?interval avg ([0-9.]+) / max ([0-9.]+) ms', txt)
    rows = [r for r in rows if int(r[1]) >= 10]
    drv = 'cached ON' if 'cached allocations on' in txt else 'cached off'
    print(f'{os.path.basename(f)}: {len(rows)} windows of 10 s with frames, {drv}')
    if rows:
        fps = [float(r[2]) for r in rows]
        mx = sorted(float(r[4]) for r in rows)
        print(f'  fps avg {sum(fps)/len(fps):.1f}, min {min(fps):.1f}, max {max(fps):.1f}; '
              f'windows with a frame >= 100 ms: {sum(1 for m in mx if m >= 100)}/{len(mx)}, >= 500 ms: '
              f'{sum(1 for m in mx if m >= 500)}; worst intervals (ms): {" ".join(f"{m:.0f}" for m in mx[-8:])}')
    for l in re.findall(r'sbumd: gllows run .*', txt)[-1:]:
        print('  ' + l)
    worst = re.findall(r'sbumd: glworst ([0-9.]+) ms: (.*)', txt)
    for w in sorted(worst, key=lambda x: -float(x[0]))[:5]:
        print(f'  glworst {w[0]} ms: {w[1]}')
    errs = Counter(re.findall(r'(MESA: error: [^\n]{0,70}|sbumd: [^\n]*(?:failed(?! 0)|error|refused)[^\n]{0,60})', txt))
    for e, n in errs.most_common(4):
        print(f'  {n}x {e}')

# ---- registry state ----
head('driver state (SYSTEM hive)')
h = os.path.join(d, 'SYSTEM')
if os.path.exists(h):
    for key, pat in (('ControlSet001\\Services\\sbwddm', r'FullGuardBoots'),
                     ('ControlSet001\\Services\\sbwddm\\Parameters', r'FullMode |GuardAutoClear|LiveSec|ScanFlip|ParallelBlt|BltNoDrain|CachedBltReads'),
                     ('ControlSet001\\Services\\sbwddm\\State', r'WddmMode|FullRefused|FullHealthy|Live_FaultCount|Live_JobsTimedOut|Live_Tdr|Live_GpuBusy|Live_Presents|Live_CacheOps'),
                     ('ControlSet001\\Services\\sbwdog\\State', r'BigCore|Seconds|Unattended|RestartAt|CapAt'),
                     ('ControlSet001\\Services\\sbthermal\\State', r'Acpm|BigCore|Midr|Max|Temp'),
                     ('ControlSet001\\Services\\sbxta\\State', r'Result|Cpu|Boots')):
        try:
            o = subprocess.run([sys.executable, hive_py, h, key], capture_output=True, text=True, timeout=60).stdout
        except Exception as e:
            o = str(e)
        sel = [l.strip() for l in o.splitlines() if re.search(pat, l)]
        print(f'{key.split(chr(92), 2)[-1]}: ' + ('; '.join(sel)[:600] if sel else '(none)'))

# ---- event logs ----
try:
    import Evtx.Evtx as evtx
except ImportError:
    print('\n(python-evtx missing: no event logs)')
    sys.exit(0)


def events(path, ids):
    if not os.path.exists(path):
        return []
    out = []
    with evtx.Evtx(path) as log:
        for r in log.records():
            try:
                x = r.xml()
            except Exception:
                continue
            m = re.search(r'<EventID[^>]*>(\d+)</EventID>', x)
            if not m or int(m.group(1)) not in ids:
                continue
            t = re.search(r'SystemTime="([^"]+)"', x)
            p = re.search(r'Provider Name="([^"]+)"', x)
            data = re.findall(r'<Data Name="([^"]+)">([^<]*)</Data>', x)
            if not data:
                data = [('', v) for v in re.findall(r'<Data>([^<]*)</Data>', x)]
            out.append((t.group(1)[:19].replace(' ', 'T') if t else '?', int(m.group(1)), p.group(1) if p else '?', data))
    out.sort()
    return out


E = os.path.join(d, 'evtx')
sysev = events(os.path.join(E, 'System.evtx'), {12, 13, 41, 1074, 6005, 6006, 6008, 7000, 7009, 7011, 7022, 7023,
                                                 7024, 7031, 7034, 7043, 109, 1001})
allt = [e[0] for e in sysev]
if since is None and allt:
    last = dt.datetime.fromisoformat(max(allt))
    since = (last - dt.timedelta(hours=12)).isoformat()[:19]
print(f'\n(events since {since} UTC)')
names = {12: 'OS start', 13: 'OS shutdown', 41: 'Kernel-Power 41 (unclean)', 1074: 'shutdown requested',
         6005: 'eventlog start', 6006: 'eventlog stop (clean)', 6008: 'UNEXPECTED shutdown', 109: 'kernel power: reboot',
         1001: 'BugCheck'}
head('boots and shutdowns (System)')
for t, i, p, data in sysev:
    if t < since or i not in names:
        continue
    extra = ''
    if i == 1074:
        extra = ' ' + ' | '.join(v for k, v in data if v)[:160]
    if i == 1001 and 'BugCheck' not in p:
        continue
    print(f'{t} {i} {names[i]}{extra}')
head('service trouble (System 7000-7043: timeouts, hangs, crashes, shutdown-delaying services)')
c = Counter()
for t, i, p, data in sysev:
    if t < since or i < 7000:
        continue
    c[(i, ' | '.join(v for k, v in data if v)[:110])] += 1
for (i, s), n in c.most_common(20):
    print(f'{n:4d}x {i}: {s}')
head('boot/shutdown performance (Diagnostics-Performance)')
for t, i, p, data in events(os.path.join(E, 'Microsoft-Windows-Diagnostics-Performance%4Operational.evtx'),
                            set(range(100, 111)) | set(range(200, 208))):
    if t < since:
        continue
    keep = ('BootTime', 'MainPathBootTime', 'BootPostBootTime', 'ShutdownTime', 'TotalTime', 'DegradationTime',
            'Name', 'FriendlyName', 'ShutdownUserPolicyTime', 'ShutdownMachinePolicyTime', 'ShutdownServicesTime',
            'ShutdownSystemSessionTime', 'ShutdownKernelTime', 'ShutdownUserSessionTime', 'ShutdownPreShutdownNotificationsTime')
    s = ', '.join(f'{k}={v}' for k, v in data if k in keep and v)
    print(f'{t} {i}: {s[:300]}')
head('app crashes and hangs (Application 1000/1002), by app')
c = Counter()
first = {}
for t, i, p, data in events(os.path.join(E, 'Application.evtx'), {1000, 1002}):
    if t < since:
        continue
    vals = [v for k, v in data if v]
    app = vals[0] if vals else '?'
    code = next((v for v in vals if v.startswith('0xc') or v.startswith('c0')), '')
    k = (i, app, code)
    c[k] += 1
    first.setdefault(k, t)
for (i, app, code), n in c.most_common(15):
    print(f'{n:4d}x {"crash" if i == 1000 else "HANG "} {app} {code} (first {first[(i, app, code)]})')
for f in ('Microsoft-Windows-Resource-Exhaustion-Detector%4Operational.evtx',):
    ev = [e for e in events(os.path.join(E, f), {2004}) if e[0] >= since]
    if ev:
        head('low memory (Resource-Exhaustion 2004)')
        for t, i, p, data in ev[-5:]:
            print(t, ' | '.join(v for k, v in data if v)[:200])
