#!/usr/bin/env python3
"""The Starbite display adapter's user-mode driver name, in an OFFLINE SYSTEM hive (docs/WIN11-GPU.md 6.108).

  hive-umdname.py IN show
  hive-umdname.py IN OUT set sbsel.dll     the selector (starbite/win/sbsel): picks sbumd-ng.dll on the catalog row
                                           "Windows 11 (new GPU)", sbumd.dll on every other row
  hive-umdname.py IN OUT set sbumd.dll     the driver itself, as the INF installed it (the way back)

The value is UserModeDriverName (REG_MULTI_SZ, the same name four times: D3D9, D3D10, D3D11, D3D12 slots) in the
display class key of our adapter (Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\NNNN whose MatchingDeviceId
is acpi\\strb0072). Only that value changes; "set" refuses unless exactly one such key exists (a walk of the
user's hive on 2026-11-16 found no other value that names the driver).

Writes go through hivexsh in the lima VM with the main checkout's tools/win/hive-service.py helpers (staging,
independent read-back, refuses a dirty hive). IN is never modified. Nothing here touches the phone.
"""
import importlib.util
import os
import shutil
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_GIT = subprocess.run(['git', '-C', _HERE, 'rev-parse', '--git-common-dir'], capture_output=True, text=True,
                      check=True).stdout.strip()
MAIN = os.environ.get('STARBITE_MAIN') or os.path.realpath(os.path.join(_HERE, _GIT, '..'))   # the main checkout
_spec = importlib.util.spec_from_file_location('hive_service', os.path.join(MAIN, 'tools/win/hive-service.py'))
hs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hs)

CLASS = '{4d36e968-e325-11ce-bfc1-08002be10318}'
DEVICE = 'acpi\\strb0072'
NAMES = ('sbsel.dll', 'sbumd.dll')
VALUE = 'UserModeDriverName'


def multi(name):
    return (7, ((name + '\0') * 4 + '\0').encode('utf-16-le'))


def text(d):
    return [s for s in d.decode('utf-16-le', 'replace').split('\0') if s]


def adapters(h):
    """[(key path, [names])] for every display class instance of our adapter that has the value."""
    c = hs.control_set(h)
    base = f'{c}\\Control\\Class\\{CLASS}'
    out = []
    _, kb = h.find(base)
    for _, sk in h.subkeys(kb):
        v = {n: (t, d) for n, t, d in h.values(sk)}
        dev = v.get('MatchingDeviceId')
        if not dev or text(dev[1])[:1] != [DEVICE]:
            continue
        if VALUE in v and v[VALUE][0] == 7:
            out.append((f'{base}\\{sk["name"]}', text(v[VALUE][1])))
    return out


def show(h):
    a = adapters(h)
    if not a:
        print('no Starbite display adapter with a UserModeDriverName in this hive')
    for k, names in a:
        what = ('the selector (new-GPU row -> sbumd-ng.dll, other rows -> sbumd.dll)' if set(names) == {'sbsel.dll'}
                else 'the driver itself, every row' if set(names) == {'sbumd.dll'} else 'UNEXPECTED')
        print(f'{k.split(chr(92), 1)[1]}: {VALUE} = {names} = {what}')
    return a


def main(a):
    if len(a) == 2 and a[1] == 'show':
        try:
            hs.check(a[0])
        except SystemExit as e:           # read-only: a dirty hive is still shown, with the warning
            print(f'({e})')
        show(hs.Hive(a[0]))
        return
    if len(a) != 4 or a[2] != 'set' or a[3] not in NAMES:
        raise SystemExit(__doc__)
    src, out, name = a[0], a[1], a[3]
    hs.check(src)
    h = hs.Hive(src)
    t = adapters(h)
    if len(t) != 1:
        raise SystemExit(f'expected exactly one Starbite display adapter key, found {len(t)}: nothing written')
    if any(set(n) - set(NAMES) for _, n in t):
        raise SystemExit(f'the adapter names a driver this tool does not know ({t[0][1]}): nothing written')
    s = hs.setvals(h, t[0][0], {VALUE: multi(name)})
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
    t2 = adapters(h2)
    # own read-back: the value is ours, and every other value of the key is byte for byte what it was
    _, k1 = h.find(t[0][0])
    _, k2 = h2.find(t[0][0])
    rest1 = sorted((n, ty, d) for n, ty, d in h.values(k1) if n != VALUE)
    rest2 = sorted((n, ty, d) for n, ty, d in h2.values(k2) if n != VALUE)
    raw = [d for n, ty, d in h2.values(k2) if n == VALUE and ty == 7]
    if len(t2) != 1 or t2[0][1] != [name] * 4 or rest1 != rest2 or raw != [multi(name)[1]]:
        os.remove(out)
        raise SystemExit('own read-back disagrees; output removed')
    show(h2)
    print(f'{out}: sha256 {hs.sha(out)}')


if __name__ == '__main__':
    main(sys.argv[1:])
