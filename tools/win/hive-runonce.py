#!/usr/bin/env python3
"""One HKLM RunOnce entry in an OFFLINE Windows SOFTWARE hive (a copy pulled from the phone).

  hive-runonce.py IN show
  hive-runonce.py IN OUT set NAME COMMAND     HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce\\NAME = COMMAND
  hive-runonce.py IN OUT remove NAME

Windows runs (and deletes) an HKLM RunOnce value once, at the next logon. HKLM RunOnce needs administrator rights to
write, and for an administrator's logon Explorer is expected to start its programs with the full token, so a script
can run DISM / compact / powercfg without a UAC prompt [unverified on this install]. The script must check that it is
elevated and do nothing otherwise (docs/WIN11-GPU-CAPS.md "C: space"). Other RunOnce values are kept byte for byte.

Writes go through hivexsh in the lima VM with tools/win/hive-service.py's helpers (staging, read-back by an
independent reader, refuses a dirty hive). IN is never modified. Nothing here touches the phone.
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

KEY = 'Microsoft\\Windows\\CurrentVersion\\RunOnce'


def show(h):
    if not hs.exists(h, KEY):
        print(f'{KEY}: no key')
        return
    _, k = h.find(KEY)
    vals = h.values(k)
    print(f'{KEY}: {len(vals)} values')
    for n, t, d in vals:
        txt = d.decode('utf-16-le', 'replace').rstrip('\0') if t in (1, 2) else d.hex()
        print(f'  {n} = ({t}) {txt}')


def main(a):
    if len(a) == 2 and a[1] == 'show':
        hs.check(a[0])
        show(hs.Hive(a[0]))
        return
    if len(a) < 4 or a[2] not in ('set', 'remove') or (a[2] == 'set' and len(a) != 5):
        raise SystemExit(__doc__)
    src, out, cmd, name = a[:4]
    if not name.replace('_', '').isalnum():
        raise SystemExit('NAME: letters, digits and _ only')
    hs.check(src)
    h = hs.Hive(src)
    if not hs.exists(h, 'Microsoft\\Windows\\CurrentVersion'):
        raise SystemExit('no Microsoft\\Windows\\CurrentVersion: is this a SOFTWARE hive?')
    os.makedirs(hs.STAGE, exist_ok=True)
    work = os.path.join(hs.STAGE, f'SOFTWARE.{os.getpid()}.{os.urandom(4).hex()}')
    shutil.copyfile(src, work)
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM does not see the staged hive as written; nothing changed')
    have = hs.exists(h, KEY)
    s = ''
    if not have:
        s += 'cd \\Microsoft\\Windows\\CurrentVersion\nadd RunOnce\n'
    if cmd == 'set':
        s += hs.setvals(h if have else None, KEY, {name: hs.sz(1, a[4])})
    else:
        if not have:
            raise SystemExit('nothing to do')
        _, k = h.find(KEY)
        keep = [(n, t, d) for n, t, d in h.values(k) if n != name]
        if len(keep) == len(h.values(k)):
            raise SystemExit('nothing to do')
        s += f'cd \\{KEY}\nsetval {len(keep)}\n'
        for n, t, d in keep:
            s += ('@' if n == '(default)' else n) + '\n' + f'hex:{t}:' + ','.join(f'{b:02x}' for b in d) + '\n'
    hs.hivexsh(work, s + 'commit\n')
    open(work + '.ls', 'w').write(f'cd \\{KEY}\nlsval\n')
    r = subprocess.run(['limactl', 'shell', 'starbite', '--', 'hivexsh', '-f', work + '.ls', work],
                       stdin=subprocess.DEVNULL, capture_output=True, text=True)
    listed = f'"{name.lower()}"' in r.stdout.lower()
    if listed != (cmd == 'set'):
        raise SystemExit(f'hivexsh read-back: {name} {"listed" if listed else "missing"}:\n{r.stdout}{r.stderr}\n{out} not written')
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
