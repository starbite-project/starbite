#!/usr/bin/env python3
"""sbxta v2 service switch (starbite/win/sbxta/README-services.md), in an OFFLINE SYSTEM hive.

  hive-sbxta-services.py IN show
  hive-sbxta-services.py IN OUT set NAME...    Services\\sbxta\\Parameters\\DisableOnM3Services = NAME... (REG_MULTI_SZ)
  hive-sbxta-services.py IN OUT off            delete that value (the next A55 boot still restores what was saved)
  hive-sbxta-services.py IN OUT param NAME N   Parameters\\NAME = N (DWORD), NAME in DisableOnM3, SwapCache
                                               (per-CPU XtaCache: DisableOnM3 0 + SwapCache 1; back: 1 + 0)

Suggested list (the user's decision): WpnUserService DiagTrack OneSyncSvc UsoSvc wuauserv.
Needs sbxta v2 (build/woa/cores/sbxta-2) installed; v1 ignores the value. Writes go through hivexsh in the lima VM
with the main checkout's tools/win/hive-service.py helpers. IN is never modified. Nothing here touches the phone.
"""
import importlib.util
import os
import shutil
import sys

MAIN = os.environ.get('STARBITE_MAIN', '/Users/dev/project-starbite')
_spec = importlib.util.spec_from_file_location('hive_service', os.path.join(MAIN, 'tools/win/hive-service.py'))
hs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hs)


def cs(h):
    _, k = h.find('Select')
    cur = {n: d for n, t, d in h.values(k)}['Current']
    return 'ControlSet%03d' % int.from_bytes(cur, 'little')


def vals(h, key):
    _, k = h.find(key)
    return {n: (t, d) for n, t, d in h.values(k)}


def show(h):
    c = cs(h)
    base = f'{c}\\Services\\sbxta'
    if not hs.exists(h, base):
        print('sbxta: not installed')
        return
    par = f'{base}\\Parameters'
    v = vals(h, par) if hs.exists(h, par) else {}
    for pn in ('DisableOnM3', 'SwapCache'):
        print(f'{pn}: ' + (str(int.from_bytes(v[pn][1], "little")) if pn in v else 'absent (default)'))
    if 'DisableOnM3Services' in v:
        names = [x for x in v['DisableOnM3Services'][1].decode('utf-16-le').split('\0') if x]
        print('DisableOnM3Services:', ' '.join(names))
    else:
        print('DisableOnM3Services: absent (switch off)')
    for sub in ('Saved', 'State'):
        k = f'{base}\\{sub}'
        if hs.exists(h, k):
            for n, (t, d) in sorted(vals(h, k).items()):
                x = hex(int.from_bytes(d, "little")) if t == 11 else int.from_bytes(d, "little") if t == 4 else d.decode('utf-16-le', 'replace').rstrip('\0')
                print(f'  {sub}\\{n} = {x}')
    for n in ('WpnUserService', 'DiagTrack', 'OneSyncSvc', 'UsoSvc', 'wuauserv'):
        k = f'{c}\\Services\\{n}'
        if hs.exists(h, k):
            s = vals(h, k).get('Start')
            print(f'  {n} Start = {int.from_bytes(s[1], "little") if s else "?"}')


def main(a):
    if len(a) == 2 and a[1] == 'show':
        hs.check(a[0])
        show(hs.Hive(a[0]))
        return
    if len(a) < 3 or a[2] not in ('set', 'off', 'param') or (a[2] == 'set' and len(a) < 4) or \
            (a[2] == 'param' and (len(a) != 5 or a[3] not in ('DisableOnM3', 'SwapCache') or a[4] not in ('0', '1'))):
        raise SystemExit(__doc__)
    src, out, cmd, names = a[0], a[1], a[2], a[3:]
    if cmd == 'param':
        names = []
    if any(('\\' in n) or ('/' in n) or not n for n in names) or len(names) > 16:
        raise SystemExit('service names only, at most 16')
    hs.check(src)
    h = hs.Hive(src)
    c = cs(h)
    base = f'{c}\\Services\\sbxta'
    par = f'{base}\\Parameters'
    if not hs.exists(h, par):
        raise SystemExit(f'{par}: missing: install sbxta first')
    v = vals(h, par)
    if cmd == 'param':
        s = hs.setvals(h, par, {a[3]: hs.dword(int(a[4]))})
    elif cmd == 'set':
        data = ('\0'.join(names) + '\0\0').encode('utf-16-le')
        s = hs.setvals(h, par, {'DisableOnM3Services': (7, data)})
    else:
        if 'DisableOnM3Services' not in v:
            raise SystemExit('already off')
        rest = [(n, t, d) for n, (t, d) in v.items() if n != 'DisableOnM3Services']
        s = f'cd \\{par}\nsetval {len(rest)}\n'
        for n, t, d in rest:
            s += ('@' if n == '(default)' else n) + '\n' + f'hex:{t}:' + ','.join(f'{b:02x}' for b in d) + '\n'
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
    v2 = vals(h2, par)
    if cmd == 'param':
        if v2.get(a[3], (0, b''))[1] != hs.dword(int(a[4]))[1]:
            os.remove(out)
            raise SystemExit('read-back disagrees; output removed')
        print(f'{par}\\{a[3]} = {a[4]}')
        print(f'{out}: sha256 {hs.sha(out)}')
        return
    if (cmd == 'set') != ('DisableOnM3Services' in v2) or \
            (cmd == 'set' and v2['DisableOnM3Services'][1] != ('\0'.join(names) + '\0\0').encode('utf-16-le')):
        os.remove(out)
        raise SystemExit('read-back disagrees; output removed')
    show(h2)
    print(f'{out}: sha256 {hs.sha(out)}')


if __name__ == '__main__':
    main(sys.argv[1:])
