"""The ext4 reader and the per-OS no-password checks, on real ext4 images made by mke2fs
(tests/fixtures/*.ext4.gz, rebuilt by tests/mk-ext4-fixtures.sh)."""
import gzip
import hashlib
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from sbinst import ext4, nopassword  # noqa: E402
from sbinst import manifest as mf  # noqa: E402

FIX = Path(__file__).resolve().parent / 'fixtures'


class Ext4Images(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = tempfile.TemporaryDirectory()
        cls.img = {}
        for n in ('nopin', 'pin', 'locked'):
            p = Path(cls.t.name) / f'{n}.ext4'
            p.write_bytes(gzip.decompress((FIX / f'linux-{n}.ext4.gz').read_bytes()))
            cls.img[n] = p

    @classmethod
    def tearDownClass(cls):
        cls.t.cleanup()

    def test_reads_files_and_hashed_dirs(self):
        fs = ext4.Ext4(self.img['nopin'])
        try:
            big = fs.read_file('/etc/big.bin')
            self.assertGreater(len(big), 300_000)
            self.assertEqual(hashlib.sha256(big).hexdigest(), fs.read_file('/etc/big.sha256').decode().strip())
            self.assertEqual(fs.read_file('/etc/many/f250'), b'file 250\n')      # 300 entries: a hashed directory
            self.assertIsNone(fs.read_file('/etc/missing'))
            self.assertIsNone(fs.read_file('/nope/x'))
        finally:
            fs.close()

    def test_not_ext4(self):
        p = Path(self.t.name) / 'junk'
        p.write_bytes(os.urandom(8192))
        with self.assertRaises(ext4.Ext4Error):
            ext4.Ext4(p)
        self.assertEqual(nopassword.linux_image(p)[0], 'OPEN')

    def test_no_password_image_ok(self):
        st, ev = nopassword.linux_image(self.img['nopin'])
        self.assertEqual(st, 'ok', ev)

    def test_pin_image_flagged(self):
        st, ev = nopassword.linux_image(self.img['pin'])
        self.assertEqual(st, 'OPEN')
        self.assertIn('PIN', ev)

    def test_locked_image_flagged(self):
        st, ev = nopassword.linux_image(self.img['locked'])
        self.assertEqual(st, 'OPEN')
        self.assertIn('locked', ev)

    def test_report_covers_every_os(self):
        ex = Path(__file__).resolve().parent.parent / 'installer-manifest.example.toml'
        with tempfile.TemporaryDirectory() as t:
            shutil.copy(ex, Path(t) / mf.MANIFEST_NAME)
            m = mf.load(t)
            for i in m.items:                 # pretend every file is present
                i.present = True
                i.path = self.img['nopin'] if i.os == 'linux' else None
            names = [c.os for c in nopassword.report(m)]
            android = lambda plan: next(c for c in nopassword.report(m, None, plan) if c.os == 'Android 17').status  # noqa: E731
            self.assertEqual(android({'android-data': 'replace'}), 'by-design')
            self.assertEqual(android({'android-data': 'keep'}), 'OPEN')     # someone else's /data: a lock may exist
        for want in ('Android 17','Linux /distros/pmos.img', 'Linux /distros/ubuntu.img', 'ChromiumOS', 'Windows 11',
                     'macOS Ventura', 'iOS 16.5', 'Engineer OS / Starbite'):
            self.assertIn(want, names)


if __name__ == '__main__':
    unittest.main()
