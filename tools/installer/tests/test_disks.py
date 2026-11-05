"""Rescue-stick disk safety: recorded diskutil plists (macOS) and fake sysfs trees (Linux)."""
import os
import plistlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from sbinst import disks  # noqa: E402
from sbinst.common import GIB  # noqa: E402

ROOT = {'DeviceIdentifier': 'disk3s1s1', 'ParentWholeDisk': 'disk3', 'Internal': True, 'BusProtocol': 'Apple Fabric',
        'APFSPhysicalStores': [{'APFSPhysicalStore': 'disk0s2'}], 'WholeDisk': False}
DISKS = {
    'disk0': {'DeviceIdentifier': 'disk0', 'WholeDisk': True, 'Internal': True, 'Removable': False, 'RemovableMedia': False,
              'BusProtocol': 'Apple Fabric', 'TotalSize': 251_000_193_024, 'MediaName': 'APPLE SSD AP0256Z'},
    'disk3': {'DeviceIdentifier': 'disk3', 'WholeDisk': True, 'Internal': True, 'BusProtocol': 'Apple Fabric',
              'TotalSize': 245_107_195_904, 'VirtualOrPhysical': 'Virtual', 'MediaName': 'AppleAPFSMedia'},
    'disk4': {'DeviceIdentifier': 'disk4', 'WholeDisk': True, 'Internal': False, 'Removable': True, 'RemovableMedia': True,
              'Ejectable': True, 'BusProtocol': 'USB', 'TotalSize': 32_017_047_552, 'MediaName': 'SanDisk Ultra Fit'},
    'disk5': {'DeviceIdentifier': 'disk5', 'WholeDisk': True, 'Internal': False, 'Removable': False, 'Ejectable': True,
              'BusProtocol': 'USB', 'TotalSize': 1_000_204_886_016, 'MediaName': 'Samsung PSSD T7'},
    'disk6': {'DeviceIdentifier': 'disk6', 'WholeDisk': True, 'Internal': False, 'BusProtocol': 'Disk Image',
              'TotalSize': 64 << 20, 'VirtualOrPhysical': 'Virtual', 'MediaName': 'Disk Image'},
    'disk7': {'DeviceIdentifier': 'disk7', 'WholeDisk': True, 'Internal': False, 'Removable': True, 'BusProtocol': 'USB',
              'TotalSize': 256_060_514_304, 'MediaName': 'SanDisk 256GB'},
    'disk8': {'DeviceIdentifier': 'disk8', 'WholeDisk': True, 'Internal': True, 'Removable': True, 'BusProtocol': 'Secure Digital',
              'TotalSize': 63_864_569_856, 'MediaName': 'SD card reader'},
    'disk4s1': {'DeviceIdentifier': 'disk4s1', 'WholeDisk': False, 'ParentWholeDisk': 'disk4', 'Internal': False,
                'BusProtocol': 'USB', 'TotalSize': 32_000_000_000},
}


def fake_diskutil(args):
    if args[0] == 'list':
        name = os.path.basename(args[-1])
        parts = [{'DeviceIdentifier': f'{name}s1', 'MountPoint': f'/Volumes/{name.upper()}'}] if name == 'disk4' else []
        return plistlib.dumps({'AllDisksAndPartitions': [{'DeviceIdentifier': name, 'Partitions': parts}]})
    if args[-1] == '/':
        return plistlib.dumps(ROOT)
    name = os.path.basename(args[-1])
    if name not in DISKS:
        raise ValueError('no such disk')
    return plistlib.dumps(DISKS[name])


def mac(dev):
    return disks.refusals(disks.mac_info(dev, fake_diskutil), system='Darwin')


class MacDisks(unittest.TestCase):
    def test_usb_stick_is_accepted(self):
        info = disks.mac_info('/dev/disk4', fake_diskutil)
        self.assertEqual(disks.refusals(info, 'Darwin'), [])
        self.assertEqual(info.model, 'SanDisk Ultra Fit')
        self.assertIn('disk4s1 on /Volumes/DISK4', info.mounted)        # shown, then unmounted by the write

    def test_rdisk_name_is_the_same_disk(self):
        self.assertEqual(mac('/dev/rdisk4'), [])

    def test_internal_system_disk_refused(self):
        why = mac('/dev/disk0')
        self.assertTrue(any('running system' in w for w in why), why)
        self.assertTrue(any('internal' in w for w in why), why)

    def test_apfs_container_of_root_refused(self):
        why = mac('/dev/disk3')
        self.assertTrue(any('running system' in w for w in why))
        self.assertTrue(any('virtual' in w for w in why))

    def test_big_usb_ssd_refused(self):
        self.assertTrue(any('larger than 256 GiB' in w for w in mac('/dev/disk5')))

    def test_256gb_stick_is_within_the_limit(self):
        self.assertEqual(mac('/dev/disk7'), [])

    def test_disk_image_refused(self):
        why = mac('/dev/disk6')
        self.assertTrue(any('not on USB' in w for w in why) and any('virtual' in w for w in why))

    def test_internal_sd_reader_refused(self):
        why = mac('/dev/disk8')
        self.assertTrue(any('internal' in w for w in why) and any('not on USB' in w for w in why))

    def test_partition_names_refused(self):
        with self.assertRaises(ValueError):
            disks.mac_info('/dev/disk4s1', fake_diskutil)
        with self.assertRaises(ValueError):
            disks.mac_info('disk4', fake_diskutil)


class LinuxDisks(unittest.TestCase):
    """A fake /sys + /proc: sda = internal NVMe-ish SATA root disk, sdb = USB stick, sdc = USB 1 TB
    SSD, sdd = USB stick with a mounted partition, sde = USB stick under an LVM root."""

    def setUp(self):
        self.t = tempfile.TemporaryDirectory()
        r = self.t.name
        self.sys, self.proc = os.path.join(r, 'sys'), os.path.join(r, 'proc')
        os.makedirs(self.proc)

        def disk(name, majmin, devpath, sectors, removable, vendor, model, parts=()):
            real = os.path.join(self.sys, 'devices', devpath, 'block', name)
            os.makedirs(os.path.join(real, 'device'))
            Path(os.path.join(real, 'size')).write_text(f'{sectors}\n')
            Path(os.path.join(real, 'removable')).write_text(f'{removable}\n')
            Path(os.path.join(real, 'dev')).write_text(f'{majmin}\n')
            Path(os.path.join(real, 'device', 'vendor')).write_text(vendor)
            Path(os.path.join(real, 'device', 'model')).write_text(model)
            os.makedirs(os.path.join(self.sys, 'block'), exist_ok=True)
            os.symlink(real, os.path.join(self.sys, 'block', name))
            os.makedirs(os.path.join(self.sys, 'dev', 'block'), exist_ok=True)
            os.symlink(real, os.path.join(self.sys, 'dev', 'block', majmin))
            for pn, pmm in parts:
                pr = os.path.join(real, pn)
                os.makedirs(pr)
                Path(os.path.join(pr, 'partition')).write_text('1\n')
                Path(os.path.join(pr, 'dev')).write_text(f'{pmm}\n')
                os.symlink(pr, os.path.join(self.sys, 'dev', 'block', pmm))
        disk('sda', '8:0', 'pci0000:00/0000:00:17.0/ata1/host0/target0:0:0/0:0:0:0', 500_118_192 * 2, 0, 'ATA', 'Samsung SSD',
             parts=[('sda1', '8:1'), ('sda2', '8:2')])
        disk('sdb', '8:16', 'pci0000:00/0000:00:14.0/usb2/2-1/2-1:1.0/host1/target1:0:0/1:0:0:0', 62_521_344, 1, 'SanDisk', 'Cruzer',
             parts=[('sdb1', '8:17')])
        disk('sdc', '8:32', 'pci0000:00/0000:00:14.0/usb2/2-2/2-2:1.0/host2/target2:0:0/2:0:0:0', 1_953_525_168, 0, 'Samsung', 'T7')
        disk('sdd', '8:48', 'pci0000:00/0000:00:14.0/usb2/2-3/2-3:1.0/host3/target3:0:0/3:0:0:0', 62_521_344, 1, 'Kingston', 'DT',
             parts=[('sdd1', '8:49')])
        disk('sde', '8:64', 'pci0000:00/0000:00:14.0/usb2/2-4/2-4:1.0/host4/target4:0:0/4:0:0:0', 62_521_344, 1, 'Generic', 'Stick',
             parts=[('sde1', '8:65')])
        # dm-0 (LVM root) sits on sde1
        dm = os.path.join(self.sys, 'devices', 'virtual', 'block', 'dm-0')
        os.makedirs(os.path.join(dm, 'slaves'))
        os.symlink(os.path.join(self.sys, 'devices', 'pci0000:00/0000:00:14.0/usb2/2-4/2-4:1.0/host4/target4:0:0/4:0:0:0',
                                'block', 'sde', 'sde1'), os.path.join(dm, 'slaves', 'sde1'))
        os.symlink(dm, os.path.join(self.sys, 'dev', 'block', '253:0'))
        os.makedirs(os.path.join(self.proc, 'self'))
        Path(os.path.join(self.proc, 'self', 'mountinfo')).write_text(
            '22 1 253:0 / / rw,relatime shared:1 - ext4 /dev/mapper/vg-root rw\n'
            '23 22 8:1 / /boot/efi rw,relatime shared:2 - vfat /dev/sda1 rw\n'
            '24 22 8:49 / /media/user/DT rw,relatime shared:3 - vfat /dev/sdd1 rw\n')
        Path(os.path.join(self.proc, 'mounts')).write_text(
            '/dev/mapper/vg-root / ext4 rw 0 0\n/dev/sda1 /boot/efi vfat rw 0 0\n/dev/sdd1 /media/user/DT vfat rw 0 0\n')
        Path(os.path.join(self.proc, 'swaps')).write_text('Filename Type Size Used Priority\n/dev/sda2 partition 8388604 0 -2\n')

    def tearDown(self):
        self.t.cleanup()

    def info(self, dev):
        return disks.linux_info(dev, self.sys, self.proc)

    def test_usb_stick_accepted(self):
        i = self.info('/dev/sdb')
        self.assertEqual(disks.refusals(i, 'Linux'), [])
        self.assertEqual((i.protocol, i.removable, i.model), ('USB', True, 'SanDisk Cruzer'))
        self.assertEqual(i.size, 62_521_344 * 512)

    def test_internal_boot_disk_refused(self):
        why = disks.refusals(self.info('/dev/sda'), 'Linux')
        self.assertTrue(any('running system' in w for w in why), why)
        self.assertTrue(any('not on USB' in w for w in why), why)

    def test_large_usb_ssd_refused(self):
        why = disks.refusals(self.info('/dev/sdc'), 'Linux')
        self.assertEqual(len(why), 1)
        self.assertIn('larger than 256 GiB', why[0])

    def test_mounted_stick_refused(self):
        why = disks.refusals(self.info('/dev/sdd'), 'Linux')
        self.assertTrue(any('mounted' in w for w in why), why)

    def test_stick_under_lvm_root_refused(self):
        why = disks.refusals(self.info('/dev/sde'), 'Linux')
        self.assertTrue(any('running system' in w for w in why), why)

    def test_bad_names_refused(self):
        for d in ('/dev/sdb1', '/dev/nvme0n1', 'sdb', '/dev/mmcblk0', '/dev/sdzzz'):
            with self.assertRaises(ValueError):
                self.info(d)

    def test_limit_is_256_gib(self):
        self.assertEqual(disks.MAX_RESCUE_DISK_BYTES, 256 * GIB)


if __name__ == '__main__':
    unittest.main()
