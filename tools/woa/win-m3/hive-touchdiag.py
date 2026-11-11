#!/usr/bin/env python3
"""Touch diagnostics (docs/WIN11-M3-CLEAN.md section 37): point the S6SY761Touch service at the diagnostic build, in an
OFFLINE SYSTEM hive, and back.

  hive-touchdiag.py IN show           ImagePath, the saved original, and every Diag* value the driver wrote
  hive-touchdiag.py IN OUT set        ImagePath -> \\SystemRoot\\System32\\drivers\\s6touchdiag.sys; the original ImagePath
                                       is kept in Services\\S6SY761Touch\\Parameters\\SbOrigImagePath
  hive-touchdiag.py IN OUT restore    ImagePath <- SbOrigImagePath, then SbOrigImagePath is deleted

The installed DriverStore file is never touched (no overwrite): only the service's ImagePath changes. The diagnostic
.sys goes to C:\\Windows\\System32\\drivers\\s6touchdiag.sys (test-signed, testsigning is on). The Diag* values live in
Enum\\ACPI\\SCSY0761\\0\\Device Parameters and are left there by restore (harmless; the old driver ignores them).

Writes go through hivexsh in the lima VM with the main checkout's tools/win/hive-service.py helpers (staging,
independent read-back, refuses a dirty hive). IN is never modified. Nothing here touches the phone.
"""
import importlib.util
import os
import shutil
import struct
import subprocess
import sys

MAIN = os.environ.get('STARBITE_MAIN', '/Users/dev/project-starbite')
_spec = importlib.util.spec_from_file_location('hive_service', os.path.join(MAIN, 'tools/win/hive-service.py'))
hs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hs)

NEW = '\\SystemRoot\\System32\\drivers\\s6touchdiag.sys'
KINDS = {1: 'press', 2: 'release', 3: 'ALL-UP i2c', 4: 'ALL-UP overflow', 5: 'queue-flush', 6: 'gap', 7: 'i2c-error',
         8: 'press-while-active', 9: 'release-inactive', 10: 'selftest'}


def cs(h):
    _, k = h.find('Select')
    cur = {n: d for n, t, d in h.values(k)}['Current']
    return 'ControlSet%03d' % int.from_bytes(cur, 'little')


def vals(h, key):
    _, k = h.find(key)
    return {n: (t, d) for n, t, d in h.values(k)}


def text(d):
    return d.decode('utf-16-le').rstrip('\0')


def show(h):
    c = cs(h)
    svc = f'{c}\\Services\\S6SY761Touch'
    v = vals(h, svc)
    print(f'{svc}: ImagePath {text(v["ImagePath"][1])}')
    p = f'{svc}\\Parameters'
    if hs.exists(h, p) and 'SbOrigImagePath' in vals(h, p):
        print(f'  SbOrigImagePath {text(vals(h, p)["SbOrigImagePath"][1])}')
    dev = f'{c}\\Enum\\ACPI\\SCSY0761\\0\\Device Parameters'
    if not hs.exists(h, dev):
        print(f'{dev}: missing')
        return
    d = vals(h, dev)
    diag = {n: x for n, x in d.items() if n.startswith('Diag')}
    if not diag:
        print('no Diag* values (the diagnostic driver has not run, or never flushed)')
        return
    for n in sorted(diag):
        t, b = diag[n]
        if t == 4:
            x = int.from_bytes(b, 'little')
            print(f'  {n} = {x}' + (f' (0x{x:08X})' if n.endswith('Status') else ''))
        elif t == 1:
            print(f'  {n} = {text(b)}')
    if 'DiagCpu' in diag:
        print('  DiagCpu (ISR runs per CPU 0..7) =', list(struct.unpack('<8I', diag['DiagCpu'][1])))
    if 'DiagSelf' in diag:
        s = struct.unpack('<8I', diag['DiagSelf'][1])
        print(f'  DiagSelf: {s[0]} SPB reads, {s[1]} errors (last 0x{s[2]:08X}), max {s[3]} us, avg {s[4]} us, '
              f'{s[5]} non-empty, {s[6]} slower than 20 ms, on CPU {s[7]}')
    if 'DiagClock' in diag:
        it, qpc, f = struct.unpack('<3Q', diag['DiagClock'][1])
        print(f'  DiagClock: interrupt time {it / 1e7:.3f} s, QPC {qpc} @ {f} Hz = {qpc / f if f else 0:.3f} s')
    if 'DiagTrace' in diag:
        w = struct.unpack('<129I', diag['DiagTrace'][1])
        head, ent = w[0], w[1:]
        n = min(head, 64)
        print(f'  DiagTrace: {head} events, last {n}:')
        prev = None
        for i in range(head - n, head):
            ms, x = ent[2 * (i % 64)], ent[2 * (i % 64) + 1]
            kind, cid, a, b = x >> 28, (x >> 24) & 0xF, (x >> 12) & 0xFFF, x & 0xFFF
            dt = '' if prev is None else f' (+{ms - prev} ms)'
            prev = ms
            extra = {6: f'{a} ms without an event', 3: f'spb {a} ms, status low 0x{b:03X}',
                     7: f'remain {a}, status low 0x{b:03X}', 10: f'errors {a}, max {b * 100} us'}.get(kind, f'x {a} y {b}')
            print(f'    {ms:>10} ms{dt:<12} {KINDS.get(kind, kind):<18} id {cid}  {extra}')


def main(a):
    if len(a) == 2 and a[1] == 'show':
        hs.check(a[0])
        show(hs.Hive(a[0]))
        return
    if len(a) != 3 or a[2] not in ('set', 'restore'):
        raise SystemExit(__doc__)
    src, out, cmd = a
    hs.check(src)
    h = hs.Hive(src)
    c = cs(h)
    svc = f'{c}\\Services\\S6SY761Touch'
    par = f'{svc}\\Parameters'
    if not hs.exists(h, par):
        raise SystemExit(f'{par}: missing: is this the phone\'s SYSTEM hive?')
    cur = text(vals(h, svc)['ImagePath'][1])
    pv = vals(h, par)
    if cmd == 'set':
        if cur.lower() == NEW.lower():
            raise SystemExit('already set')
        if 'SbOrigImagePath' in pv:
            raise SystemExit('SbOrigImagePath already present: restore first')
        s = hs.setvals(h, svc, {'ImagePath': hs.sz(2, NEW)})
        s += hs.setvals(h, par, {'SbOrigImagePath': hs.sz(2, cur)})
        want = NEW
    else:
        if 'SbOrigImagePath' not in pv:
            raise SystemExit('no SbOrigImagePath: nothing to restore')
        want = text(pv['SbOrigImagePath'][1])
        s = hs.setvals(h, svc, {'ImagePath': hs.sz(2, want)})
        rest = [(n, t, d) for n, (t, d) in pv.items() if n != 'SbOrigImagePath']
        s += f'cd \\{par}\nsetval {len(rest)}\n'
        for n, t, d in rest:
            s += ('@' if n == '(default)' else n) + '\n' + f'hex:{t}:' + ','.join(f'{b:02x}' for b in d) + '\n'
    os.makedirs(hs.STAGE, exist_ok=True)
    work = os.path.join(hs.STAGE, f'SYSTEM.{os.getpid()}.{os.urandom(4).hex()}')
    shutil.copyfile(src, work)
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM does not see the staged hive as written; nothing changed')
    hs.hivexsh(work, s + 'commit\n')
    open(work + '.ls', 'w').write(f'cd \\{svc}\nlsval ImagePath\n')
    r = subprocess.run(['limactl', 'shell', 'starbite', '--', 'hivexsh', '-f', work + '.ls', work],
                       stdin=subprocess.DEVNULL, capture_output=True, text=True)
    if want.lower() not in r.stdout.lower():
        raise SystemExit(f'hivexsh read-back: ImagePath is {r.stdout.strip()!r}, wanted {want}; {out} not written')
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM and the Mac disagree on the edited hive; nothing written')
    shutil.copyfile(work, out)
    for f in (work, work + '.hivexsh', work + '.ls'):
        if os.path.exists(f):
            os.remove(f)
    hs.check(out)
    h2 = hs.Hive(out)
    if text(vals(h2, svc)['ImagePath'][1]).lower() != want.lower():
        os.remove(out)
        raise SystemExit('own read-back disagrees; output removed')
    if cmd == 'restore' and 'SbOrigImagePath' in vals(h2, par):
        os.remove(out)
        raise SystemExit('SbOrigImagePath still present; output removed')
    show(h2)
    print(f'{out}: sha256 {hs.sha(out)}')


if __name__ == '__main__':
    main(sys.argv[1:])
