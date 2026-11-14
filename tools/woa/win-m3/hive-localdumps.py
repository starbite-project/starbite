#!/usr/bin/env python3
"""T3 (docs/WIN11-M3-X86.md section 3): WER LocalDumps for the emulated test programs, in an OFFLINE SOFTWARE hive.

  hive-localdumps.py IN show [EXE...]
  hive-localdumps.py IN OUT set [EXE...]     LocalDumps\\<EXE> for each EXE (default sbcpu-x64.exe sbcpu-x86.exe):
                                       DumpType 2 (full), DumpFolder C:\\sbdumps (REG_EXPAND_SZ), DumpCount 2
  hive-localdumps.py IN OUT remove [EXE...]  delete those app keys again; LocalDumps itself goes too when it has no
                                       values and no other subkeys (pull-1111 had no LocalDumps key at all)

Key: HKLM\\SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps\\<exe>. WER writes <exe>.<pid>.dmp into
DumpFolder when that process crashes (the folder is created by WER if missing). Diagnostic only: nothing else changes.

Writes go through hivexsh in the lima VM with the main checkout's tools/win/hive-service.py helpers (same staging,
read-back by an independent reader, refuses a dirty hive). IN is never modified. Nothing here touches the phone.
"""
import importlib.util
import os
import shutil
import subprocess
import sys

MAIN = os.environ.get('STARBITE_MAIN', '/Users/dev/project-starbite')
_spec = importlib.util.spec_from_file_location('hive_service', os.path.join(MAIN, 'tools/win/hive-service.py'))
hs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hs)

LD = 'Microsoft\\Windows\\Windows Error Reporting\\LocalDumps'
APPS = ('sbcpu-x64.exe', 'sbcpu-x86.exe')     # default; e.g. GeometryDash.exe on the command line
VALUES = {'DumpType': hs.dword(2), 'DumpFolder': hs.sz(2, 'C:\\sbdumps'), 'DumpCount': hs.dword(2)}


def show(h):
    if not hs.exists(h, LD):
        print(f'{LD}: no key')
        return
    _, k = h.find(LD)
    print(f'{LD}: {len(h.values(k))} values')
    for app in APPS:
        key = f'{LD}\\{app}'
        if hs.exists(h, key):
            _, a = h.find(key)
            vals = {n: (t, d) for n, t, d in h.values(a)}
            desc = []
            for n, (t, d) in sorted(vals.items()):
                desc.append(f'{n}=' + (str(int.from_bytes(d, "little")) if t == 4 else d.decode('utf-16-le').rstrip('\0')))
            print(f'  {app}: ' + ', '.join(desc))
        else:
            print(f'  {app}: no key')


def subkey_names(h, k):
    return [nk['name'] for _, nk in h.subkeys(k)]


def ensure(h, path, made):
    s, cur = '', ''
    for part in path.split('\\'):
        nxt = f'{cur}\\{part}' if cur else part
        if nxt not in made and not hs.exists(h, nxt):
            made.add(nxt)
            s += f'cd \\{cur}\nadd {part}\n'
        cur = nxt
    return s


def main(a):
    global APPS
    if len(a) >= 2 and a[1] == 'show' and len(a) > 2:
        APPS = tuple(a[2:])
        a = a[:2]
    elif len(a) > 3 and a[2] in ('set', 'remove'):
        APPS = tuple(a[3:])
        a = a[:3]
    if any(('\\' in x) or ('/' in x) or not x.lower().endswith('.exe') for x in APPS):
        raise SystemExit('EXE names only, e.g. GeometryDash.exe')
    if len(a) == 2 and a[1] == 'show':
        hs.check(a[0])
        show(hs.Hive(a[0]))
        return
    if len(a) != 3 or a[2] not in ('set', 'remove'):
        raise SystemExit(__doc__)
    src, out, cmd = a
    hs.check(src)
    h = hs.Hive(src)
    if not hs.exists(h, 'Microsoft\\Windows\\Windows Error Reporting'):
        raise SystemExit('no Windows Error Reporting key: is this a SOFTWARE hive?')
    os.makedirs(hs.STAGE, exist_ok=True)
    work = os.path.join(hs.STAGE, f'SOFTWARE.{os.getpid()}.{os.urandom(4).hex()}')
    shutil.copyfile(src, work)
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM does not see the staged hive as written; nothing changed')
    s, made = '', set()
    for app in APPS:
        key = f'{LD}\\{app}'
        have = hs.exists(h, key)
        if cmd == 'set':
            s += ensure(h, key, made) + hs.setvals(h if have else None, key, VALUES)
        elif have:
            s += f'cd \\{key}\ndel\n'
    if cmd == 'remove' and hs.exists(h, LD):          # LocalDumps empty apart from ours: remove it too
        _, k = h.find(LD)
        others = [n for n in subkey_names(h, k) if n.lower() not in [x.lower() for x in APPS]]
        if not h.values(k) and not others:
            s = f'cd \\{LD}\ndel\n'
    if not s:
        raise SystemExit('nothing to do')
    hs.hivexsh(work, s + 'commit\n')
    for app in APPS:                                # independent read-back
        open(work + '.ls', 'w').write(f'cd \\{LD}\nls\n')
        r = subprocess.run(['limactl', 'shell', 'starbite', '--', 'hivexsh', '-f', work + '.ls', work],
                           stdin=subprocess.DEVNULL, capture_output=True, text=True)
        listed = app.lower() in r.stdout.lower()
        if listed != (cmd == 'set'):
            raise SystemExit(f'hivexsh read-back: {app} {"listed" if listed else "missing"}; {out} not written')
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM and the Mac disagree on the edited hive; nothing written')
    shutil.copyfile(work, out)
    for f in (work, work + '.hivexsh', work + '.ls'):
        if os.path.exists(f):
            os.remove(f)
    hs.check(out)
    show(hs.Hive(out))
    print(f'{out}: sha256 {hs.sha(out)}')


if __name__ == '__main__':
    main(sys.argv[1:])
