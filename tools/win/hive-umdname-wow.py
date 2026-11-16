#!/usr/bin/env python3
"""The Starbite display adapter's user-mode driver name FOR 32-BIT PROGRAMS, in an OFFLINE SYSTEM hive.

  hive-umdname-wow.py IN show
  hive-umdname-wow.py IN OUT set [sbumd.dll]   32-bit x86 programs load C:\\Windows\\SysWOW64\\sbumd.dll (the x86 build,
                                               build/win/umd-x86/sbumd.dll) as their D3D10/11 driver
  hive-umdname-wow.py IN OUT clear             the value is removed: 32-bit programs get no hardware driver from us
                                               (Windows gives them its software renderer), as before. The way back.

The value is UserModeDriverNameWow (REG_MULTI_SZ, the same name four times: D3D9, D3D10, D3D11, D3D12 slots, as
UserModeDriverName has it) in the display class key of our adapter (Control\\Class\\{4d36e968-e325-11ce-bfc1-
08002be10318}\\NNNN whose MatchingDeviceId is acpi\\strb0072): the key tools/win/hive-umdname.py edits. The 64-bit
value UserModeDriverName (sbumd.dll or the selector sbsel.dll) is never touched; "set" and "clear" refuse unless
exactly one such key exists and it has a UserModeDriverName. A bare file name is resolved by a 32-bit process in
its own system directory, which is SysWOW64: the 64-bit and the 32-bit driver can both be called sbumd.dll.

Modelled on tools/win/hive-umdname.py: writes go through hivexsh in the lima VM with the main checkout's
tools/win/hive-service.py helpers (staging in build/win/hive, refuses a dirty hive), then an own read-back: the
value is what was asked for and every other value of the key is byte for byte what it was. IN is never modified.
Nothing here touches the phone. (2026-11-16; round trip tried on a copy of a pulled hive, never booted.)
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
NAMES = ('sbumd.dll',)                 # the x86 driver's file name in SysWOW64
VALUE = 'UserModeDriverNameWow'
NATIVE = 'UserModeDriverName'          # must exist (that is how the key is recognised); never written


def multi(name):
    return (7, ((name + '\0') * 4 + '\0').encode('utf-16-le'))


def text(d):
    return [s for s in d.decode('utf-16-le', 'replace').split('\0') if s]


def adapters(h):
    """[(key path, native names, wow names or None, wow type or None)] for every display class instance of our
    adapter that has a UserModeDriverName."""
    c = hs.control_set(h)
    base = f'{c}\\Control\\Class\\{CLASS}'
    out = []
    _, kb = h.find(base)
    for _, sk in h.subkeys(kb):
        v = {n: (t, d) for n, t, d in h.values(sk)}
        dev = v.get('MatchingDeviceId')
        if not dev or text(dev[1])[:1] != [DEVICE]:
            continue
        if NATIVE in v and v[NATIVE][0] == 7:
            wow = v.get(VALUE)
            out.append((f'{base}\\{sk["name"]}', text(v[NATIVE][1]), text(wow[1]) if wow else None,
                        wow[0] if wow else None))
    return out


def show(h):
    a = adapters(h)
    if not a:
        print('no Starbite display adapter with a UserModeDriverName in this hive')
    for k, native, wow, wtype in a:
        if wow is None:
            what = 'absent = 32-bit programs get no hardware driver from us (software rendering)'
        elif wtype == 7 and set(wow) == {'sbumd.dll'} and len(wow) == 4:
            what = f'{wow} = 32-bit x86 programs load SysWOW64\\sbumd.dll'
        else:
            what = f'{wow} (type {wtype}) = UNEXPECTED'
        print(f'{k.split(chr(92), 1)[1]}: {VALUE} {what}; {NATIVE} = {native} (not touched by this tool)')
    return a


def script(h, key, add):
    """hivexsh "setval" replaces every value of a key: re-emit them all byte for byte (hex:<type>:..) without
    VALUE, then ADD ({name: (type, bytes)}, empty for "clear"). hs.setvals can add and replace, not remove."""
    _, k = h.find(key)
    vals = [(n, t, d) for n, t, d in h.values(k) if n != VALUE and n not in add]
    vals += [(n, t, d) for n, (t, d) in add.items()]
    s = f'cd \\{key}\nsetval {len(vals)}\n'
    for n, t, d in vals:
        s += ('@' if n == '(default)' else n) + '\n' + f'hex:{t}:' + ','.join(f'{b:02x}' for b in d) + '\n'
    return s


def main(a):
    if len(a) == 2 and a[1] == 'show':
        try:
            hs.check(a[0])
        except SystemExit as e:           # read-only: a dirty hive is still shown, with the warning
            print(f'({e})')
        show(hs.Hive(a[0]))
        return
    if len(a) == 3 and a[2] == 'set':
        a = a + ['sbumd.dll']
    if not ((len(a) == 4 and a[2] == 'set' and a[3] in NAMES) or (len(a) == 3 and a[2] == 'clear')):
        raise SystemExit(__doc__)
    src, out, name = a[0], a[1], (a[3] if a[2] == 'set' else None)
    if os.path.realpath(src) == os.path.realpath(out):
        raise SystemExit('IN and OUT are the same file: nothing written')
    hs.check(src)
    h = hs.Hive(src)
    t = adapters(h)
    if len(t) != 1:
        raise SystemExit(f'expected exactly one Starbite display adapter key, found {len(t)}: nothing written')
    key, _, wow, wtype = t[0]
    if wow is not None and (wtype != 7 or set(wow) - set(NAMES)):
        raise SystemExit(f'the adapter already names a 32-bit driver this tool does not know ({wow}, type {wtype}): '
                         'nothing written')
    _, k1 = h.find(key)
    if len(h.values(k1)) - (1 if wow is not None else 0) + (1 if name else 0) < 1:
        raise SystemExit('the key would be left without values: nothing written')
    s = script(h, key, {VALUE: multi(name)} if name else {})
    os.makedirs(hs.STAGE, exist_ok=True)
    work = os.path.join(hs.STAGE, f'SYSTEM.{os.getpid()}.{os.urandom(4).hex()}')
    try:
        shutil.copyfile(src, work)
        if hs.vm_sha(work) != hs.sha(work):
            raise SystemExit('the VM does not see the staged hive as written; nothing changed')
        hs.hivexsh(work, s + 'commit\n')
        if hs.vm_sha(work) != hs.sha(work):
            raise SystemExit('the VM and the Mac disagree on the edited hive; nothing written')
        shutil.copyfile(work, out)
    finally:
        for f in (work, work + '.hivexsh'):          # this run's own staging files only
            if os.path.exists(f):
                os.remove(f)
    hs.check(out)
    h2 = hs.Hive(out)
    t2 = adapters(h2)
    # own read-back: the value is ours (or gone), and every other value of the key is byte for byte what it was
    _, k2 = h2.find(key)
    rest1 = sorted((n, ty, d) for n, ty, d in h.values(k1) if n != VALUE)
    rest2 = sorted((n, ty, d) for n, ty, d in h2.values(k2) if n != VALUE)
    raw = [(ty, d) for n, ty, d in h2.values(k2) if n == VALUE]
    good = len(t2) == 1 and t2[0][0] == key and t2[0][1] == t[0][1] and rest1 == rest2
    good = good and (raw == [multi(name)] if name else raw == [])
    if not good:
        os.remove(out)
        raise SystemExit('own read-back disagrees; output removed')
    show(h2)
    print(f'{out}: sha256 {hs.sha(out)}')


if __name__ == '__main__':
    main(sys.argv[1:])
