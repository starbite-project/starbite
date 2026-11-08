#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Install / remove / read the Starbite Display IddCx device in OFFLINE Windows hives (copies pulled from the phone).

  idd-hive.py system   IN OUT install     SYSTEM hive: Enum\\Root\\StarbiteDisplay\\0000 + its Display class driver key
  idd-hive.py software IN OUT install     SOFTWARE hive: the UMDF service StarbiteDisplay (WUDF\\Services)
  idd-hive.py system   IN OUT remove      undo (device key + the class key it points to)
  idd-hive.py software IN OUT remove      undo (the UMDF service)
  idd-hive.py status   SYSTEM             print the device keys and the driver's Sb* status values (read only)

What pnputil + a devcon-style install would leave behind, reduced to what PnP and UMDF read at boot (INSTALL.md
has the why for each value). Both hives are needed; the DLL goes to Windows\\System32\\drivers\\UMDF\\.
IN is never modified; OUT is a new file. Refuses a hive whose two sequence numbers differ (not shut down
cleanly). Writes go through hivexsh in the lima VM "starbite" (the helpers of tools/win/hive-service.py), staged
under build/link/idd/hive-stage/; afterwards the result is read back with tools/woa/hive.py. Nothing here
touches the phone.
"""
import importlib.util
import os
import shutil
import struct
import sys

REPO = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../..'))
_spec = importlib.util.spec_from_file_location('hive_service', os.path.join(REPO, 'tools/win/hive-service.py'))
hs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hs)  # also puts tools/woa on sys.path
from hive import Hive, dump  # noqa: E402

STAGE = os.path.join(REPO, 'build/link/idd/hive-stage')

SVC = 'StarbiteDisplay'                       # UMDF service name (INF UmdfService)
DEV = 'Root\\StarbiteDisplay'                 # hardware id and Enum\Root\<id>
INST = '0000'
DISPLAY = '{4d36e968-e325-11ce-bfc1-08002be10318}'
WUDF = 'Microsoft\\Windows NT\\CurrentVersion\\WUDF\\Services'
UMDF_MINOR = 33                               # built against UMDF 2.33 (tools/win/build.sh)


def multi(*items):
    return (7, ('\0'.join(items) + '\0\0').encode('utf-16-le'))


def qword(v):
    return (11, struct.pack('<Q', v))


def stage(src):
    hs.check(src)
    os.makedirs(STAGE, exist_ok=True)
    work = os.path.join(STAGE, f'hive.{os.getpid()}.{os.urandom(4).hex()}')  # fresh name: virtiofs caches names
    shutil.copyfile(src, work)
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM does not see the staged hive as written; nothing changed')
    return work


def finish(work, out):
    if hs.vm_sha(work) != hs.sha(work):
        raise SystemExit('the VM and the Mac disagree on the edited hive; nothing written')
    shutil.copyfile(work, out)
    for f in (work, work + '.hivexsh'):
        if os.path.exists(f):
            os.remove(f)
    hs.check(out)


def add_key(parent, name, vals):
    return f'cd \\{parent}\nadd {name}\n' + hs.setvals(None, f'{parent}\\{name}', vals)


def system_install(src, out):
    h = Hive(src)
    cs = hs.control_set(h)
    for need in (f'{cs}\\Services\\WUDFRd', f'{cs}\\Services\\IndirectKmd', f'{cs}\\Control\\Class\\{DISPLAY}',
                 f'{cs}\\Enum\\Root'):
        if not hs.exists(h, need):
            raise SystemExit(f'missing {need}: not the Windows this was written for; nothing changed')
    ekey = f'{cs}\\Enum\\{DEV}'
    if hs.exists(h, ekey):
        raise SystemExit(f'{ekey} exists already (remove first); nothing changed')
    ckey = f'{cs}\\Control\\Class\\{DISPLAY}'
    _, ck = h.find(ckey)
    used = {sk['name'] for _, sk in h.subkeys(ck)}
    drv = next(f'{i:04d}' for i in range(10000) if f'{i:04d}' not in used)

    # Driver (software) key, as the INF install writes it: Control\Class\{Display}\NNNN
    s = add_key(ckey, drv, {
        'DriverDesc': hs.sz(1, 'Starbite Display'), 'ProviderName': hs.sz(1, 'Starbite'),
        'DriverVersion': hs.sz(1, '0.1.0.0'), 'DriverDate': hs.sz(1, '11-7-2026'),
        'DriverDateData': qword(0x01DD6E4AEE8F8000),   # FILETIME 2026-11-07 00:00 UTC
        'InfPath': hs.sz(1, 'StarbiteDisplay.inf'), 'InfSection': hs.sz(1, 'StarbiteDisplay_Install'),
        'MatchingDeviceId': hs.sz(1, DEV.lower())})
    # Device instance: Enum\Root\StarbiteDisplay\0000 (what DIF_REGISTERDEVICE + the INF's .NT/.HW/.Services make)
    s += add_key(f'{cs}\\Enum\\Root', DEV.split('\\')[1], {})
    s += add_key(ekey, INST, {
        'HardwareID': multi(DEV), 'Service': hs.sz(1, 'WUDFRd'), 'UpperFilters': multi('IndirectKmd'),
        'ClassGUID': hs.sz(1, DISPLAY), 'Class': hs.sz(1, 'Display'), 'Driver': hs.sz(1, f'{DISPLAY}\\{drv}'),
        'DeviceDesc': hs.sz(1, 'Starbite Display'), 'Mfg': hs.sz(1, 'Starbite'), 'ConfigFlags': hs.dword(0),
        'Capabilities': hs.dword(0), 'ContainerID': hs.sz(1, '{00000000-0000-0000-FFFF-FFFFFFFFFFFF}')})
    # Hardware key (HKR of .HW / the .Wdf directives): Device Parameters\WUDF
    s += add_key(f'{ekey}\\{INST}', 'Device Parameters', {})
    s += add_key(f'{ekey}\\{INST}\\Device Parameters', 'WUDF', {
        'DriverList': multi(SVC),                    # UmdfServiceOrder
        'KernelModeClientPolicy': hs.dword(1),       # UmdfKernelModeClientPolicy = AllowKernelModeClients
        'FileObjectPolicy': hs.dword(1),             # UmdfFileObjectPolicy = AllowNullAndUnknownFileObjects
        'MethodNeitherAction': hs.dword(1),          # UmdfMethodNeitherAction = Copy
        'FsContextUsePolicy': hs.dword(1),           # UmdfFsContextUsePolicy = CanUseFsContext2
        'HostProcessSharingEnabled': hs.dword(0)})   # UmdfHostProcessSharing = ProcessSharingDisabled
    s += 'commit\n'
    work = stage(src)
    hs.hivexsh(work, s)
    finish(work, out)
    h2 = Hive(out)
    for k in (f'{ckey}\\{drv}', ekey):
        _, n = h2.find(k)
        dump(h2, n, k, True)


def system_remove(src, out):
    h = Hive(src)
    cs = hs.control_set(h)
    ekey = f'{cs}\\Enum\\{DEV}'
    if not hs.exists(h, ekey):
        raise SystemExit(f'no {ekey}; nothing changed')
    s = ''
    if hs.exists(h, f'{ekey}\\{INST}'):
        _, k = h.find(f'{ekey}\\{INST}')
        drv = dict((n, d) for n, _, d in h.values(k)).get('Driver')
        if drv:
            drv = drv.decode('utf-16-le').rstrip('\0')
            if drv.lower().startswith(DISPLAY) and hs.exists(h, f'{cs}\\Control\\Class\\{drv}'):
                s += f'cd \\{cs}\\Control\\Class\\{drv}\ndel\n'
    s += f'cd \\{ekey}\ndel\ncommit\n'
    work = stage(src)
    hs.hivexsh(work, s)
    finish(work, out)
    print(f'{ekey}: {"STILL THERE" if hs.exists(Hive(out), ekey) else "removed"}')


def software_install(src, out):
    h = Hive(src)
    if not hs.exists(h, f'{WUDF}\\IddCx0102'):
        raise SystemExit(f'missing {WUDF}\\IddCx0102 (the IddCx class extension); nothing changed')
    key = f'{WUDF}\\{SVC}'
    if hs.exists(h, key):
        raise SystemExit(f'{key} exists already (remove first); nothing changed')
    # Same shape as the in-box RdpIdd entry on the phone (an IddCx driver too).
    s = add_key(WUDF, SVC, {
        'WdfMajorVersion': hs.dword(2), 'WdfMinorVersion': hs.dword(UMDF_MINOR),
        'ImagePath': hs.sz(2, f'%SystemRoot%\\System32\\drivers\\UMDF\\{SVC}.dll'),
        'WdfExtensions': multi('IddCx0102')}) + 'commit\n'
    work = stage(src)
    hs.hivexsh(work, s)
    finish(work, out)
    h2 = Hive(out)
    _, n = h2.find(key)
    dump(h2, n, key, True)


def software_remove(src, out):
    key = f'{WUDF}\\{SVC}'
    if not hs.exists(Hive(src), key):
        raise SystemExit(f'no {key}; nothing changed')
    work = stage(src)
    hs.hivexsh(work, f'cd \\{key}\ndel\ncommit\n')
    finish(work, out)
    print(f'{key}: {"STILL THERE" if hs.exists(Hive(out), key) else "removed"}')


def status(src):
    h = Hive(src)
    cs = hs.control_set(h)
    ekey = f'{cs}\\Enum\\{DEV}'
    if not hs.exists(h, ekey):
        raise SystemExit(f'no {ekey}: the device is not installed in this hive')
    _, k = h.find(ekey)
    dump(h, k, ekey, True)
    _, i = h.find(f'{ekey}\\{INST}')
    drv = dict((n, d) for n, _, d in h.values(i)).get('Driver')
    if drv:
        p = f'{cs}\\Control\\Class\\{drv.decode("utf-16-le").rstrip(chr(0))}'
        if hs.exists(h, p):
            _, c = h.find(p)
            dump(h, c, p, True)


def main(a):
    if len(a) == 2 and a[0] == 'status':
        return status(a[1])
    if len(a) != 4:
        raise SystemExit(__doc__)
    which, src, out, cmd = a
    fn = {('system', 'install'): system_install, ('system', 'remove'): system_remove,
          ('software', 'install'): software_install, ('software', 'remove'): software_remove}.get((which, cmd))
    if not fn:
        raise SystemExit(__doc__)
    if os.path.realpath(src) == os.path.realpath(out):
        raise SystemExit('OUT must be a new file')
    fn(src, out)


if __name__ == '__main__':
    main(sys.argv[1:])
