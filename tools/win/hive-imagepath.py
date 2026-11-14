#!/usr/bin/env python3
"""Point a driver service at another .sys and back, in an OFFLINE SYSTEM hive (docs/WIN11-GPU.md 6.83).

  hive-imagepath.py IN show SERVICE
  hive-imagepath.py IN OUT set SERVICE PATH    Services\\SERVICE\\ImagePath = PATH (REG_EXPAND_SZ); the original is
                                               kept in Services\\SERVICE\\Parameters\\SbOrigImagePath (first set only)
  hive-imagepath.py IN OUT restore SERVICE     ImagePath <- SbOrigImagePath, then SbOrigImagePath is deleted

The same scheme as tools/woa/win-m3/hive-touchdiag.py, for any service (first use: Exynos9810Hsi2c -> sbhsi2c.sys).
The DriverStore file is never touched. Writes go through hivexsh in the lima VM with the main checkout's
tools/win/hive-service.py helpers (staging, read-back, refuses a dirty hive). IN is never modified; nothing here
touches the phone.
"""
import importlib.util
import os
import shutil
import sys

MAIN = os.environ.get('STARBITE_MAIN', '/Users/dev/project-starbite')
_spec = importlib.util.spec_from_file_location('hive_service', os.path.join(MAIN, 'tools/win/hive-service.py'))
hs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hs)


def vals(h, key):
    _, k = h.find(key)
    return {n: (t, d) for n, t, d in h.values(k)}


def text(d):
    return d.decode('utf-16-le').rstrip('\0')


def setvals_without(h, key, name):
    """hivexsh setval re-emitting every value of KEY except NAME (hive-service.py's setvals in the main checkout has
    no remove option)."""
    _, k = h.find(key)
    keep = [(n, t, d) for n, t, d in h.values(k) if n != name]
    s = f'cd \\{key}\nsetval {len(keep)}\n'
    for n, t, d in keep:
        s += ('@' if n == '(default)' else n) + '\n' + f'hex:{t}:' + ','.join(f'{b:02x}' for b in d) + '\n'
    return s


def show(h, svc):
    c = hs.control_set(h)
    key = f'{c}\\Services\\{svc}'
    v = vals(h, key)
    print(f'{key}: ImagePath {text(v["ImagePath"][1])}')
    p = key + '\\Parameters'
    if hs.exists(h, p) and 'SbOrigImagePath' in vals(h, p):
        print(f'  SbOrigImagePath {text(vals(h, p)["SbOrigImagePath"][1])}')


def main(a):
    if len(a) == 3 and a[1] == 'show':
        hs.check(a[0])
        show(hs.Hive(a[0]), a[2])
        return
    if not ((len(a) == 5 and a[2] == 'set') or (len(a) == 4 and a[2] == 'restore')):
        raise SystemExit(__doc__)
    src, out, cmd, svc = a[:4]
    hs.check(src)
    h = hs.Hive(src)
    c = hs.control_set(h)
    key = f'{c}\\Services\\{svc}'
    par = key + '\\Parameters'
    if not hs.exists(h, key):
        raise SystemExit(f'no service {svc}')
    cur = text(vals(h, key)['ImagePath'][1])
    orig = text(vals(h, par)['SbOrigImagePath'][1]) if hs.exists(h, par) and 'SbOrigImagePath' in vals(h, par) else None
    s = ''
    if cmd == 'set':
        want = a[4]
        if not hs.exists(h, par):
            s += f'cd \\{key}\nadd Parameters\n'
        if orig is None:
            s += hs.setvals(h if hs.exists(h, par) else None, par, {'SbOrigImagePath': hs.sz(1, cur)})
        s += hs.setvals(h, key, {'ImagePath': hs.sz(2, want)})
    else:
        if orig is None:
            raise SystemExit('no SbOrigImagePath: nothing to restore')
        want = orig
        s += hs.setvals(h, key, {'ImagePath': hs.sz(2, want)})
        s += setvals_without(h, par, 'SbOrigImagePath')
    os.makedirs(hs.STAGE, exist_ok=True)
    work = os.path.join(hs.STAGE, f'SYSTEM.{os.getpid()}.{os.urandom(4).hex()}')
    shutil.copyfile(src, work)
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM does not see the staged hive as written; nothing changed')
    hs.hivexsh(work, s + 'commit\n')
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM and the Mac disagree on the edited hive; nothing written')
    shutil.copyfile(work, out)
    for f in (work, work + '.hivexsh'):
        if os.path.exists(f):
            os.remove(f)
    hs.check(out)
    h2 = hs.Hive(out)
    got = text(vals(h2, key)['ImagePath'][1])
    if got.lower() != want.lower():
        os.remove(out)
        raise SystemExit(f'own read-back: ImagePath {got!r}, wanted {want!r}; output removed')
    if cmd == 'restore' and hs.exists(h2, par) and 'SbOrigImagePath' in vals(h2, par):
        os.remove(out)
        raise SystemExit('SbOrigImagePath still present; output removed')
    show(h2, svc)
    print(f'{out}: sha256 {hs.sha(out)}')


if __name__ == '__main__':
    main(sys.argv[1:])
