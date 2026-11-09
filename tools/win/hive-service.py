#!/usr/bin/env python3
"""Add, disable or remove a driver service in an OFFLINE Windows SYSTEM hive (a copy pulled from the phone).

  hive-service.py IN OUT add NAME [--image PATH] [--start N] [--type N] [--oneshot 0|1] [--display TEXT]
  hive-service.py IN OUT disable NAME          Start = 4 (the service is skipped at boot; files stay)
  hive-service.py IN OUT remove NAME           delete the service key
  hive-service.py IN OUT hiberboot 0|1         Control\\Session Manager\\Power\\HiberbootEnabled (fast startup)
  hive-service.py IN OUT crashdump 0|3         Control\\CrashControl\\CrashDumpEnabled: 3 = small memory dumps into
                                               %SystemRoot%\\Minidump (the S9's Windows had 0: the T15 restart
                                               bugcheck left no dump); tools/win/kdump.py reads them
  hive-service.py IN OUT param NAME [--root] VALUE=N [VALUE=N ...]
                                               DWORDs in Services\\NAME\\Parameters (kept others); --root: in
                                               Services\\NAME itself (e.g. sbwddm's FullGuardBoots=0)
  hive-service.py IN OUT display-reset 1       delete Control\\GraphicsDrivers\\Configuration and Connectivity:
                                               Windows' saved display layouts, rebuilt at the next boot from the
                                               driver's preferred mode (resolution, orientation and scale return
                                               to their defaults)
  hive-service.py IN OUT display-bind SERVICE [--umd DLL] [--preemption 0|1]
                                               bind ACPI\\STRB0072 (the S9's GPU0) to SERVICE: Enum Service, the
                                               driver key's InstalledDisplayDrivers, and UserModeDriverName =
                                               DLL x4 with --umd (removed without: display-only drivers have
                                               none); --preemption sets GraphicsDrivers\\Scheduler\\EnablePreemption
  hive-service.py IN check                     header sanity only (both sequence numbers equal = clean)

IN is never modified; OUT is a new file. Defaults for add: Type 1 (kernel driver), Start 2 (auto: loaded by
the Service Control Manager once the system is up, when C: exists), ErrorControl 0 (a failure to load is
ignored and never triggers recovery), ImagePath \\SystemRoot\\System32\\drivers\\NAME.sys,
Parameters\\OneShot = 1 (hello-kmdf then sets its own Start to 4 on load: one load per arming).
Writes go through hivexsh (libhivex) in the lima VM "starbite"; the hive is staged under build/win/hive/,
which the VM sees at the same path. Afterwards the key is read back with tools/woa/hive.py.

Only edit a hive from a cleanly shut-down Windows: if the primary file's two sequence numbers differ, the
transaction logs (SYSTEM.LOG1/LOG2) hold newer data that Windows would replay over this edit -> refused.
Nothing here touches the phone.
"""
import hashlib
import os
import shutil
import struct
import subprocess
import sys

REPO = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../..'))
sys.path.insert(0, os.path.join(REPO, 'tools/woa'))
from hive import Hive, dump  # noqa: E402

STAGE = os.path.join(REPO, 'build/win/hive')


def check(path):
    d = open(path, 'rb').read(4096)
    if d[:4] != b'regf':
        raise SystemExit(f'{path}: not a registry hive')
    seq1, seq2 = struct.unpack_from('<II', d, 4)
    csum = 0
    for i in range(0, 0x1fc, 4):
        csum ^= struct.unpack_from('<I', d, i)[0]
    ok = csum == struct.unpack_from('<I', d, 0x1fc)[0]
    print(f'{os.path.basename(path)}: sequence {seq1}/{seq2} ({"clean" if seq1 == seq2 else "DIRTY"}), '
          f'header checksum {"ok" if ok else "BAD"}, {os.path.getsize(path)} bytes')
    if seq1 != seq2 or not ok:
        raise SystemExit('refusing: the hive is not in a clean state (shut Windows down cleanly and pull it again)')


def control_set(h):
    _, sel = h.find('Select')
    cur = dict((n, d) for n, _, d in h.values(sel))['Current']
    return f'ControlSet{struct.unpack("<I", cur)[0]:03d}'


def exists(h, path):
    try:
        h.find(path)
        return True
    except SystemExit:
        return False


def setvals(h, key, updates, remove=()):
    """hivexsh "setval" REPLACES every value of a key, so re-emit the existing ones byte for byte
    (hex:<type>:..) with UPDATES {name: (type, bytes)} applied on top, and REMOVE left out."""
    vals = []
    if h is not None:
        _, k = h.find(key)
        vals = [(n, t, d) for n, t, d in h.values(k) if n not in updates and n not in remove]
    vals += [(n, t, d) for n, (t, d) in updates.items()]
    s = f'cd \\{key}\nsetval {len(vals)}\n'
    for n, t, d in vals:
        s += ('@' if n == '(default)' else n) + '\n' + f'hex:{t}:' + ','.join(f'{b:02x}' for b in d) + '\n'
    return s


def dword(v):
    return (4, struct.pack('<I', v))


def sz(typ, text):
    return (typ, (text + '\0').encode('utf-16-le'))


def multi_sz(items):
    return (7, b''.join((x + '\0').encode('utf-16-le') for x in items) + '\0'.encode('utf-16-le'))


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def vm_sha(path):
    r = subprocess.run(['limactl', 'shell', 'starbite', '--', 'sha256sum', path], stdin=subprocess.DEVNULL,
                       capture_output=True, text=True, check=True)
    return r.stdout.split()[0]


def hivexsh(path, script):
    os.makedirs(STAGE, exist_ok=True)
    sp = path + '.hivexsh'
    open(sp, 'w').write(script)
    r = subprocess.run(['limactl', 'shell', 'starbite', '--', 'hivexsh', '-w', '-f', sp, path],
                       stdin=subprocess.DEVNULL, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f'hivexsh failed:\n{r.stdout}{r.stderr}\nscript:\n{script}')


def main(a):
    if len(a) >= 2 and a[1] == 'check':
        check(a[0])
        return
    if len(a) < 4:
        raise SystemExit(__doc__)
    src, out, cmd, arg = a[0], a[1], a[2], a[3]
    opts = dict(zip(a[4::2], a[5::2]))
    check(src)
    h = Hive(src)
    cs = control_set(h)
    os.makedirs(STAGE, exist_ok=True)
    # A new name every run: the VM's virtiofs view caches names, and a reused name can resolve to the
    # previous run's file (seen: an edit landing in the renamed output of the run before).
    work = os.path.join(STAGE, f'SYSTEM.{os.getpid()}.{os.urandom(4).hex()}')
    shutil.copyfile(src, work)
    if vm_sha(work) != sha(work):
        raise SystemExit('the VM does not see the staged hive as written; nothing changed')

    if cmd in ('add', 'disable', 'remove'):
        key = f'{cs}\\Services\\{arg}'
        have = exists(h, key)
        if cmd == 'add':
            image = opts.get('--image', f'\\SystemRoot\\System32\\drivers\\{arg}.sys')
            vals = {'Type': dword(int(opts.get('--type', '1'))), 'Start': dword(int(opts.get('--start', '2'))),
                    'ErrorControl': dword(0), 'ImagePath': sz(2, image),
                    'DisplayName': sz(1, opts.get('--display', 'Starbite ' + arg))}
            par = {'OneShot': dword(int(opts.get('--oneshot', '1')))}
            # an existing key is updated in place (its other values kept); never delete + re-add
            if have:
                s = setvals(h, key, vals)
            else:
                s = f'cd \\{cs}\\Services\nadd {arg}\n' + setvals(None, key, vals)
            if exists(h, key + '\\Parameters'):
                s += setvals(h, key + '\\Parameters', par)
            else:
                s += f'cd \\{key}\nadd Parameters\n' + setvals(None, key + '\\Parameters', par)
            s += 'commit\n'
        elif cmd == 'disable':
            if not have:
                raise SystemExit(f'no service {arg}')
            s = setvals(h, key, {'Start': dword(4)}) + 'commit\n'
        else:
            if not have:
                raise SystemExit(f'no service {arg}')
            s = f'cd \\{key}\ndel\ncommit\n'
        hivexsh(work, s)
        shown = key if cmd != 'remove' else f'{cs}\\Services'
    elif cmd == 'param':
        key = f'{cs}\\Services\\{arg}'
        if not exists(h, key):
            raise SystemExit(f'no service {arg}')
        vals = {}
        root = '--root' in a[4:]
        for kv in [x for x in a[4:] if x != '--root']:
            n, _, v = kv.partition('=')
            if not n or not v:
                raise SystemExit(f'bad VALUE=N: {kv}')
            vals[n] = dword(int(v, 0))
        if not vals:
            raise SystemExit(__doc__)
        if root:
            s = setvals(h, key, vals)
        elif exists(h, key + '\\Parameters'):
            s = setvals(h, key + '\\Parameters', vals)
        else:
            s = f'cd \\{key}\nadd Parameters\n' + setvals(None, key + '\\Parameters', vals)
        hivexsh(work, s + 'commit\n')
        shown = key if root else key + '\\Parameters'
    elif cmd == 'display-reset':
        gd = f'{cs}\\Control\\GraphicsDrivers'
        s = ''
        for sub in ('Configuration', 'Connectivity'):
            if exists(h, f'{gd}\\{sub}'):
                s += f'cd \\{gd}\\{sub}\ndel\n'
        if not s:
            raise SystemExit('nothing to reset (no Configuration/Connectivity key)')
        hivexsh(work, s + 'commit\n')
        shown = gd
    elif cmd == 'display-bind':
        enum = f'{cs}\\Enum\\ACPI\\STRB0072\\0'
        if not exists(h, enum):
            raise SystemExit('no ACPI\\STRB0072 device in this hive')
        _, k = h.find(enum)
        drv = dict((n, d) for n, _, d in h.values(k)).get('Driver')
        if not drv:
            raise SystemExit('STRB0072 has no driver key (never installed?)')
        drv = drv.decode('utf-16-le').rstrip('\0')
        cls = f'{cs}\\Control\\Class\\{drv}'
        if not exists(h, cls):
            raise SystemExit(f'driver key {cls} missing')
        if not exists(h, f'{cs}\\Services\\{arg}'):
            raise SystemExit(f'no service {arg}: add it first')
        s = setvals(h, enum, {'Service': sz(1, arg)})
        upd = {'InstalledDisplayDrivers': multi_sz([arg])}
        if '--umd' in opts:
            upd['UserModeDriverName'] = multi_sz([opts['--umd']] * 4)
            s += setvals(h, cls, upd)
        else:
            s += setvals(h, cls, upd, remove=('UserModeDriverName',))
        if '--preemption' in opts:
            sch = f'{cs}\\Control\\GraphicsDrivers\\Scheduler'
            pre = {'EnablePreemption': dword(int(opts['--preemption']))}
            if exists(h, sch):
                s += setvals(h, sch, pre)
            else:
                s += f'cd \\{cs}\\Control\\GraphicsDrivers\nadd Scheduler\n' + setvals(None, sch, pre)
        hivexsh(work, s + 'commit\n')
        shown = cls
    elif cmd == 'crashdump':
        if arg not in ('0', '3'):
            raise SystemExit('crashdump 0|3')
        key = f'{cs}\\Control\\CrashControl'
        hivexsh(work, setvals(h, key, {'CrashDumpEnabled': dword(int(arg))}) + 'commit\n')
        shown = key
    elif cmd == 'hiberboot':
        key = f'{cs}\\Control\\Session Manager\\Power'
        hivexsh(work, setvals(h, key, {'HiberbootEnabled': dword(int(arg))}) + 'commit\n')
        shown = key
    else:
        raise SystemExit(__doc__)

    # second, independent reader: hivexsh itself must list (or not list) the service
    if cmd in ('add', 'disable', 'remove'):
        open(work + '.ls', 'w').write(f'cd \\{cs}\\Services\nls\n')
        r = subprocess.run(['limactl', 'shell', 'starbite', '--', 'hivexsh', '-f', work + '.ls', work],
                           stdin=subprocess.DEVNULL, capture_output=True, text=True, check=True)
        listed = arg.lower() in (x.strip().lower() for x in r.stdout.splitlines())
        if listed != (cmd != 'remove'):
            raise SystemExit(f'hivexsh read-back: {arg} {"listed" if listed else "missing"}; {out} not written')
    if vm_sha(work) != sha(work):
        raise SystemExit('the VM and the Mac disagree on the edited hive; nothing written')
    shutil.copyfile(work, out)
    for f in (work, work + '.hivexsh', work + '.ls'):
        if os.path.exists(f):
            os.remove(f)
    check(out)
    h2 = Hive(out)
    if cmd == 'display-bind':
        _, k = h2.find(f'{cs}\\Enum\\ACPI\\STRB0072\\0')
        print('STRB0072 Service =', dict((n, d) for n, _, d in h2.values(k))['Service'].decode('utf-16-le').rstrip('\0'))
    if cmd == 'remove':
        print(f'{arg}: {"STILL THERE" if exists(h2, key) else "removed"}')
    else:
        _, k = h2.find(shown)
        dump(h2, k, shown, cmd == 'add')


if __name__ == '__main__':
    main(sys.argv[1:])
