"""Which host disk may become a Starbite rescue USB stick. Read-only: diskutil info (macOS) or
/sys + /proc (Linux). Refuses internal and system disks, anything not on USB, disk images, and
anything larger than 256 GiB. The functions take their inputs as arguments so the tests can feed
recorded diskutil plists and fake sysfs trees."""
import os
import plistlib
import re
import subprocess
from dataclasses import dataclass, field

from .common import MAX_RESCUE_DISK_BYTES, human


@dataclass
class DiskInfo:
    dev: str                       # as given (/dev/disk4, /dev/sdb)
    name: str                      # disk4, sdb
    model: str = ''
    size: int = 0
    whole: bool = True
    internal: bool = None
    removable: bool = None
    protocol: str = ''
    virtual: bool = False
    holds_system: bool = False     # the running system (/ or its APFS physical store) is on it
    mounted: list = field(default_factory=list)
    notes: list = field(default_factory=list)

    def rows(self):
        return [('device', self.dev), ('model', self.model or '?'), ('size', f'{human(self.size)} ({self.size} bytes)'),
                ('bus', self.protocol or '?'), ('internal', self.internal), ('removable', self.removable),
                ('mounted', ', '.join(self.mounted) or 'nothing')]


def mac_dev_name(dev):
    m = re.fullmatch(r'/dev/r?(disk\d+)', dev)
    return m.group(1) if m else None


def linux_dev_name(dev):
    m = re.fullmatch(r'/dev/(sd[a-z]{1,2})', dev)
    return m.group(1) if m else None


def _diskutil(args):
    return subprocess.run(['diskutil', *args], capture_output=True, check=True).stdout


def mac_info(dev, diskutil=_diskutil):
    name = mac_dev_name(dev)
    if not name:
        raise ValueError(f'{dev}: give a whole disk as /dev/diskN (from `diskutil list external`)')
    d = plistlib.loads(diskutil(['info', '-plist', f'/dev/{name}']))
    info = DiskInfo(dev=dev, name=name,
                    model=(d.get('MediaName') or d.get('IORegistryEntryName') or '').strip(),
                    size=int(d.get('TotalSize') or d.get('Size') or 0),
                    whole=bool(d.get('WholeDisk')),
                    internal=d.get('Internal'),
                    removable=bool(d.get('Removable') or d.get('RemovableMedia') or d.get('Ejectable')),
                    protocol=d.get('BusProtocol', ''),
                    virtual=d.get('VirtualOrPhysical') == 'Virtual' or d.get('BusProtocol') == 'Disk Image')
    # the running system: / and the physical stores of its APFS container
    root = plistlib.loads(diskutil(['info', '-plist', '/']))
    sys_disks = {root.get('ParentWholeDisk', '')}
    for st in root.get('APFSPhysicalStores', []) or []:
        sys_disks.add(re.sub(r's\d+$', '', st.get('APFSPhysicalStore', '')))
    info.holds_system = name in sys_disks
    try:
        lst = plistlib.loads(diskutil(['list', '-plist', f'/dev/{name}']))
        for disk in lst.get('AllDisksAndPartitions', []):
            for p in disk.get('Partitions', []) + disk.get('APFSVolumes', []):
                if p.get('MountPoint'):
                    info.mounted.append(f'{p.get("DeviceIdentifier")} on {p["MountPoint"]}')
                    if p['MountPoint'] in ('/', '/System/Volumes/Data'):
                        info.holds_system = True
    except (subprocess.CalledProcessError, plistlib.InvalidFileException):
        info.notes.append('diskutil list failed')
    return info


def _read(path, default=''):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def _disk_of(sysfs, majmin):
    """The whole disk(s) behind a block device number (follows dm/md slaves)."""
    link = os.path.join(sysfs, 'dev', 'block', majmin)
    if not os.path.exists(link):
        return set()
    real = os.path.realpath(link)
    name = os.path.basename(real)
    if os.path.exists(os.path.join(real, 'partition')):
        name = os.path.basename(os.path.dirname(real))
        real = os.path.dirname(real)
    slaves = os.path.join(real, 'slaves')
    if os.path.isdir(slaves) and os.listdir(slaves):
        out = set()
        for s in os.listdir(slaves):
            out |= _disk_of(sysfs, _read(os.path.join(slaves, s, 'dev')))
        return out
    return {name}


def linux_info(dev, sysfs='/sys', proc='/proc'):
    name = linux_dev_name(dev)
    if not name:
        raise ValueError(f'{dev}: give a whole USB disk as /dev/sdX (from `lsblk -d -o NAME,TRAN,SIZE,MODEL`)')
    base = os.path.join(sysfs, 'block', name)
    if not os.path.isdir(base):
        raise ValueError(f'{dev}: no such disk ({base} missing)')
    real = os.path.realpath(base)
    info = DiskInfo(dev=dev, name=name,
                    model=' '.join(x for x in (_read(f'{base}/device/vendor'), _read(f'{base}/device/model')) if x),
                    size=int(_read(f'{base}/size', '0') or 0) * 512,
                    removable=_read(f'{base}/removable') == '1',
                    protocol='USB' if '/usb' in real else ('virtual' if '/virtual/' in real else 'not USB'))
    info.virtual = '/virtual/' in real
    info.internal = info.protocol != 'USB'
    # the running system: the disk(s) under / (mountinfo gives major:minor, which survives LVM/LUKS)
    sys_disks = set()
    for line in _read(os.path.join(proc, 'self', 'mountinfo')).splitlines():
        f = line.split()
        if len(f) > 4 and f[4] in ('/', '/boot', '/boot/efi', '/usr', '/home'):
            sys_disks |= _disk_of(sysfs, f[2])
    for line in _read(os.path.join(proc, 'swaps')).splitlines()[1:]:
        d = line.split()[0] if line.split() else ''
        if d.startswith(f'/dev/{name}'):
            sys_disks.add(name)
    info.holds_system = name in sys_disks
    for line in _read(os.path.join(proc, 'mounts')).splitlines():
        f = line.split()
        if len(f) > 1 and re.fullmatch(rf'/dev/{name}\d*', f[0]):
            info.mounted.append(f'{f[0]} on {f[1]}')
    return info


def refusals(info, system=None):
    """Every reason this disk must not be written. Empty list = acceptable."""
    why = []
    if not info.whole:
        why.append('it is a partition, not a whole disk')
    if info.holds_system:
        why.append('it holds the running system')
    if info.internal:
        why.append('it is an internal disk')
    if info.protocol != 'USB':
        why.append(f'it is not on USB (bus: {info.protocol or "unknown"})')
    if info.virtual:
        why.append('it is a virtual disk / disk image')
    if info.size <= 0:
        why.append('it reports no size (no medium?)')
    if info.size > MAX_RESCUE_DISK_BYTES:
        why.append(f'it is larger than 256 GiB ({human(info.size)}): not a rescue stick')
    if (system or os.uname().sysname) == 'Linux' and info.mounted:
        why.append('it has mounted file systems (unmount them first: ' + ', '.join(info.mounted) + ')')
    return why


def disk_info(dev, fake_dir=None):
    """Dispatch by host OS; SBINST_FAKE_DISKS (rehearsal) replaces diskutil/sysfs by recorded data."""
    fake_dir = fake_dir or os.environ.get('SBINST_FAKE_DISKS')
    if fake_dir:
        if mac_dev_name(dev):
            def fake_diskutil(args):
                key = 'root' if args[-1] == '/' else os.path.basename(args[-1])
                kind = 'list' if args[0] == 'list' else 'info'
                p = os.path.join(fake_dir, 'diskutil', f'{kind}-{key}.plist')
                if not os.path.exists(p):
                    if kind == 'list':
                        return plistlib.dumps({'AllDisksAndPartitions': []})
                    raise ValueError(f'{dev}: no such disk')
                return open(p, 'rb').read()
            return mac_info(dev, fake_diskutil)
        return linux_info(dev, os.path.join(fake_dir, 'sys'), os.path.join(fake_dir, 'proc'))
    if os.uname().sysname == 'Darwin':
        return mac_info(dev)
    return linux_info(dev)
