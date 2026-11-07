#!/usr/bin/env python3
"""Add, disable or remove a driver service in an OFFLINE Windows SYSTEM hive (a copy pulled from the phone).

  hive-service.py IN OUT add NAME [--image PATH] [--start N] [--type N] [--oneshot 0|1] [--display TEXT]
  hive-service.py IN OUT disable NAME          Start = 4 (the service is skipped at boot; files stay)
  hive-service.py IN OUT remove NAME           delete the service key
  hive-service.py IN OUT hiberboot 0|1         Control\\Session Manager\\Power\\HiberbootEnabled (fast startup)
  hive-service.py IN OUT usbfn VID:PID         Control USBFN key: one configuration, GenericUsbFn, bulk IN 0x81 + OUT 0x01
  hive-service.py IN OUT ufx-autologger NAME   ETW boot trace of UfxSynopsys + Ufx01000 into LogFiles WMI NAME.etl
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


def setvals(h, key, updates):
    """hivexsh "setval" REPLACES every value of a key, so re-emit the existing ones byte for byte
    (hex:<type>:..) with UPDATES {name: (type, bytes)} applied on top."""
    vals = []
    if h is not None:
        _, k = h.find(key)
        vals = [(n, t, d) for n, t, d in h.values(k) if n not in updates]
    vals += [(n, t, d) for n, (t, d) in updates.items()]
    s = f'cd \\{key}\nsetval {len(vals)}\n'
    for n, t, d in vals:
        s += ('@' if n == '(default)' else n) + '\n' + f'hex:{t}:' + ','.join(f'{b:02x}' for b in d) + '\n'
    return s


def dword(v):
    return (4, struct.pack('<I', v))


def sz(typ, text):
    return (typ, (text + '\0').encode('utf-16-le'))


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
    elif cmd == 'hiberboot':
        key = f'{cs}\\Control\\Session Manager\\Power'
        hivexsh(work, setvals(h, key, {'HiberbootEnabled': dword(int(arg))}) + 'commit\n')
        shown = key
    elif cmd == 'usbfn':
        # USB function controller configuration (docs/WIN11-USB.md 5.1): VID:PID, one configuration with
        # Windows' GenericUsbFn on a vendor interface with bulk IN 0x81 + bulk OUT 0x01 (512 bytes).
        vid, pid = (int(x, 16) for x in arg.split(':'))
        base = f'{cs}\\Control\\USBFN'
        if exists(h, base):
            raise SystemExit(f'{base} exists already: edit by hand, nothing changed')
        desc = bytes.fromhex('090400000 2FF000000 0705810200020 0 0705010200020 0'.replace(' ', ''))
        multi = ('GenericUsbFn\0\0').encode('utf-16-le')
        s = f'cd \\{cs}\\Control\nadd USBFN\n'
        s += setvals(None, base, {'idVendor': dword(vid), 'idProduct': dword(pid),
                                  'ManufacturerString': sz(1, 'Starbite'), 'ProductString': sz(1, 'Starbite Link'),
                                  'CurrentConfiguration': sz(1, 'StarbiteLink'), 'IncludeDefaultCfg': dword(0)})
        s += f'cd \\{base}\nadd Configurations\ncd \\{base}\\Configurations\nadd StarbiteLink\n'
        s += setvals(None, base + '\\Configurations\\StarbiteLink', {'InterfaceList': (7, multi)})
        s += f'cd \\{base}\nadd Interfaces\ncd \\{base}\\Interfaces\nadd GenericUsbFn\n'
        s += setvals(None, base + '\\Interfaces\\GenericUsbFn',
                     {'InterfaceGUID': sz(1, '{d274dae6-37b0-4c76-bf10-3a9681346dd1}'),
                      'InterfaceDescriptor': (3, desc)})
        s += 'commit\n'
        hivexsh(work, s)
        shown = base + '\\Interfaces\\GenericUsbFn'
    elif cmd == 'bind':
        # Offline PnP install of a KMDF driver onto an existing device instance, what pnputil would leave
        # behind: Services\<svc> (demand start, Parameters, Parameters\Wdf\KmdfLibraryVersion), the device's
        # Enum key pointing at it, and its Control\Class\{guid}\NNNN driver key.
        #   bind ENUM_INSTANCE --service S --class-guid {G} --class NAME --desc TEXT [--kmdf 1.33]
        #        [--param NAME=DWORD ...]   (--param repeatable as Name=Value,Name=Value)
        inst = arg
        svc = opts['--service']
        guid = opts['--class-guid'].lower()
        ekey = f'{cs}\\Enum\\{inst}'
        if not exists(h, ekey):
            raise SystemExit(f'no device {ekey}')
        skey = f'{cs}\\Services\\{svc}'
        if exists(h, skey):
            raise SystemExit(f'{skey} exists already: nothing changed')
        ckey = f'{cs}\\Control\\Class\\{guid}'
        _, ck = h.find(ckey)
        used = {sk['name'] for _, sk in h.subkeys(ck)}
        n = next(i for i in range(10000) if f'{i:04d}' not in used)
        drv = f'{n:04d}'
        params = {}
        for kv in opts.get('--param', '').split(','):
            if kv:
                k_, v_ = kv.split('=')
                params[k_] = dword(int(v_, 0))
        s = f'cd \\{cs}\\Services\nadd {svc}\n'
        s += setvals(None, skey, {'Type': dword(1), 'Start': dword(3), 'ErrorControl': dword(0),
                                  'ImagePath': sz(2, f'\\SystemRoot\\System32\\drivers\\{svc}.sys'),
                                  'DisplayName': sz(1, opts.get('--desc', svc))})
        s += f'cd \\{skey}\nadd Parameters\n' + setvals(None, skey + '\\Parameters', params)
        s += f'cd \\{skey}\\Parameters\nadd Wdf\n' + setvals(None, skey + '\\Parameters\\Wdf',
                                                         {'KmdfLibraryVersion': sz(1, opts.get('--kmdf', '1.33'))})
        s += f'cd \\{ckey}\nadd {drv}\n' + setvals(None, f'{ckey}\\{drv}', {
            'DriverDesc': sz(1, opts.get('--desc', svc)), 'ProviderName': sz(1, 'Starbite'),
            'DriverVersion': sz(1, '0.1.0.0'), 'MatchingDeviceId': sz(1, inst.rsplit('\\', 1)[0].lower())})
        s += setvals(h, ekey, {'Service': sz(1, svc), 'ClassGUID': sz(1, guid), 'Class': sz(1, opts.get('--class', 'System')),
                               'Driver': sz(1, f'{guid}\\{drv}'), 'ConfigFlags': dword(0),
                               'DeviceDesc': sz(1, opts.get('--desc', svc))})
        s += 'commit\n'
        hivexsh(work, s)
        shown = ekey
    elif cmd == 'ufx-autologger':
        # ETW boot autologger for UfxSynopsys + Ufx01000 (docs/WIN11-USB.md 5.2); arg = session name. Undo: delete the key.
        base = f'{cs}\\Control\\WMI\\Autologger'
        key = f'{base}\\{arg}'
        if exists(h, key):
            raise SystemExit(f'{key} exists already: nothing changed')
        s = f'cd \\{base}\nadd {arg}\n'
        s += setvals(None, key, {'Start': dword(1), 'GUID': sz(1, '{696fc72d-68b5-47e1-8806-aac101ebe127}'),
                                 'FileName': sz(1, f'%SystemRoot%\\System32\\LogFiles\\WMI\\{arg}.etl'),
                                 'LogFileMode': dword(0x10000001), 'MaxFileSize': dword(64),
                                 'BufferSize': dword(64), 'FlushTimer': dword(1), 'ClockType': dword(2)})
        for prov in ('{49b12c7c-4bd5-4f93-bb75-30fce739600b}', '{e98ebdbf-3058-4784-8521-47860b1d2b8e}'):
            s += f'cd \\{key}\nadd {prov}\n'
            s += setvals(None, f'{key}\\{prov}', {'Enabled': dword(1), 'EnableLevel': dword(5),
                                                   'EnableFlags': dword(0xFFFFFFFF),
                                                   'MatchAnyKeyword': (11, struct.pack('<Q', 0xFFFFFFFFFFFFFFFF))})
        s += 'commit\n'
        hivexsh(work, s)
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
    if cmd == 'remove':
        print(f'{arg}: {"STILL THERE" if exists(h2, key) else "removed"}')
    else:
        _, k = h2.find(shown)
        dump(h2, k, shown, cmd == 'add')


if __name__ == '__main__':
    main(sys.argv[1:])
