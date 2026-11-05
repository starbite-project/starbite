"""Manifest parsing, the destination allowlist, bundle verification and the hash updater."""
import hashlib
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from sbinst import manifest as mf  # noqa: E402
from sbinst import tomlmini  # noqa: E402
from sbinst.common import Refused  # noqa: E402

HEAD = '''format = 1
name = "test bundle"
[card]
p1 = { type = 0x07, start = 8192, sectors = 182878208 }
p2 = { type = 0xda, start = 182886400, sectors = 67112960 }
'''


def item(key, dest, file=None, size=4096, sha='0' * 64, extra=''):
    f = f'file = "{file}"\nsize = {size}\nsha256 = "{sha}"\n' if file else ''
    return f'[[item]]\nkey = "{key}"\ndest = "{dest}"\n{f}{extra}\n'


class Dests(unittest.TestCase):
    def test_allowed(self):
        for s, kind in (('part:BOOT', 'part'), ('part:SYSTEM', 'part'), ('slot:USERDATA@0', 'slot'),
                        ('store:ufs', 'store'), ('store:sd', 'store'), ('card:lba:216442880', 'card-lba'),
                        ('card:file:/distros/pmos.img', 'card-file')):
            self.assertEqual(mf.parse_dest(s).kind, kind)
            self.assertEqual(str(mf.parse_dest(s)), s)

    def test_protected_refused(self):
        for s in ('part:EFS', 'part:CPEFS', 'part:PARAM', 'part:UP_PARAM', 'part:KEYSTORAGE', 'part:PERSISTENT',
                  'part:STEADY', 'part:BOTA0', 'part:BOTA2', 'part:DTB', 'part:DTBO', 'part:sda', 'part:sdb', 'part:sde'):
            with self.assertRaises(Refused) as e:
                mf.parse_dest(s)
            self.assertIn('protected', str(e.exception))

    def test_not_on_allowlist(self):
        for s in ('part:USERDATA', 'part:RADIO', 'part:CACHE', 'part:boot', 'card:lba:0', 'card:lba:1000',
                  'card:file:distros/x', 'card:file:/a/../b', 'slot:USERDATA@17179869184', 'slot:USERDATA@100',
                  'store:usb', 'gpt:sda', 'raw:/dev/block/sda', 'part:'):
            with self.assertRaises(Refused, msg=s):
                mf.parse_dest(s)


class Load(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory()
        self.b = Path(self.t.name)

    def tearDown(self):
        self.t.cleanup()

    def write(self, body, files=()):
        (self.b / mf.MANIFEST_NAME).write_text(HEAD + body)
        for name, data in files:
            p = self.b / name
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)

    def test_good_bundle_verifies(self):
        data = os.urandom(8192)
        self.write(item('boot', 'part:BOOT', 'boot/boot.img', 8192, hashlib.sha256(data).hexdigest()),
                   [('boot/boot.img', data)])
        m = mf.load(self.b)
        self.assertEqual(mf.verify_bundle(m), [])
        self.assertTrue(m.items[0].present)

    def test_missing_file(self):
        self.write(item('boot', 'part:BOOT', 'boot/boot.img'))
        self.assertIn('boot: missing file boot/boot.img', mf.verify_bundle(mf.load(self.b)))

    def test_missing_optional_is_not_a_problem(self):
        self.write(item('win', 'part:WINDOWS', 'win.img', extra='optional = true'))
        m = mf.load(self.b)
        self.assertEqual(mf.verify_bundle(m), [])
        self.assertFalse(m.items[0].present)

    def test_size_mismatch(self):
        self.write(item('boot', 'part:BOOT', 'b.img', 4096), [('b.img', bytes(8192))])
        self.assertTrue(any('8192 bytes, the manifest says 4096' in p for p in mf.verify_bundle(mf.load(self.b))))

    def test_sha_mismatch(self):
        self.write(item('boot', 'part:BOOT', 'b.img', 4096, 'a' * 64), [('b.img', bytes(4096))])
        self.assertTrue(any('sha256' in p for p in mf.verify_bundle(mf.load(self.b))))

    def test_file_outside_bundle(self):
        with self.assertRaises(Refused):
            self.write(item('boot', 'part:BOOT', '../x.img'))
            mf.load(self.b)

    def test_symlink_out_of_bundle(self):
        outside = Path(self.t.name).parent / f'sbinst-outside-{os.getpid()}.img'
        outside.write_bytes(bytes(4096))
        try:
            self.write(item('boot', 'part:BOOT', 'b.img', 4096, hashlib.sha256(bytes(4096)).hexdigest()))
            os.symlink(outside, self.b / 'b.img')
            self.assertTrue(any('outside the bundle' in p for p in mf.verify_bundle(mf.load(self.b))))
        finally:
            outside.unlink()

    def test_protected_dest_refused_at_load(self):
        self.write(item('efs', 'part:EFS', 'efs.img'))
        with self.assertRaises(Refused):
            mf.load(self.b)

    def test_duplicate_key_and_dest(self):
        self.write(item('a', 'part:BOOT', 'a.img') + item('a', 'part:RECOVERY', 'b.img'))
        with self.assertRaises(Refused):
            mf.load(self.b)
        self.write(item('a', 'part:BOOT', 'a.img') + item('b', 'part:BOOT', 'b.img'))
        with self.assertRaises(Refused):
            mf.load(self.b)

    def test_odm_only_fresh(self):
        self.write(item('odm', 'part:ODM', 'odm.img'))
        with self.assertRaises(Refused):
            mf.load(self.b)
        self.write(item('odm', 'part:ODM', extra='action = "mkfs-metadata"'))
        m = mf.load(self.b)
        self.assertTrue(m.items[0].data)            # /metadata is always treated as user data

    def test_card_range_outside_p2_or_overlapping(self):
        self.write(item('x', 'card:lba:8192', 'x.img', 1 << 20))
        with self.assertRaises(Refused):
            mf.load(self.b)
        self.write(item('x', 'card:lba:216442880', 'x.img', 4 << 20) + item('y', 'card:lba:216444928', 'y.img', 1 << 20))
        with self.assertRaises(Refused):
            mf.load(self.b)

    def test_unaligned_sizes(self):
        self.write(item('x', 'part:SYSTEM', 'x.img', 1000))
        with self.assertRaises(Refused):
            mf.load(self.b)
        self.write(item('x', 'card:lba:216442880', 'x.img', 4096))
        with self.assertRaises(Refused):
            mf.load(self.b)

    def test_format_and_min_card(self):
        (self.b / mf.MANIFEST_NAME).write_text('format = 2\n')
        with self.assertRaises(Refused):
            mf.load(self.b)
        (self.b / mf.MANIFEST_NAME).write_text('format = 1\n[card]\nmin_bytes = 32000000000\n')
        with self.assertRaises(Refused):
            mf.load(self.b)

    def test_update_hashes(self):
        data = os.urandom(4096)
        self.write('# a comment that must survive\n' + item('boot', 'part:BOOT', 'b.img', 1, 'f' * 64), [('b.img', data)])
        self.assertEqual(mf.update_hashes(self.b), ['b.img'])
        text = (self.b / mf.MANIFEST_NAME).read_text()
        self.assertIn('# a comment that must survive', text)
        m = mf.load(self.b)
        self.assertEqual((m.items[0].size, m.items[0].sha256), (4096, hashlib.sha256(data).hexdigest()))
        self.assertEqual(mf.verify_bundle(m), [])

    def test_hash_cache(self):
        data = os.urandom(4096)
        self.write(item('boot', 'part:BOOT', 'b.img', 4096, hashlib.sha256(data).hexdigest()), [('b.img', data)])
        cache = mf.HashCache(self.b / 'cache.json')
        self.assertEqual(mf.verify_bundle(mf.load(self.b), cache), [])
        self.assertTrue((self.b / 'cache.json').is_file())
        # a changed file (new mtime/size) is hashed again, not taken from the cache
        (self.b / 'b.img').write_bytes(os.urandom(4096))
        self.assertTrue(any('sha256' in p for p in mf.verify_bundle(mf.load(self.b), mf.HashCache(self.b / 'cache.json'))))


class ExampleManifest(unittest.TestCase):
    def test_example_parses(self):
        ex = Path(__file__).resolve().parent.parent / 'installer-manifest.example.toml'
        with tempfile.TemporaryDirectory() as t:
            shutil.copy(ex, Path(t) / mf.MANIFEST_NAME)
            m = mf.load(t)
            self.assertGreater(len(m.items), 10)
            self.assertEqual(m.item('boot').dest.name, 'BOOT')


class TomlFallback(unittest.TestCase):
    def test_fallback_matches_tomllib(self):
        ex = (Path(__file__).resolve().parent.parent / 'installer-manifest.example.toml').read_text()
        try:
            import tomllib
        except ImportError:
            self.skipTest('no tomllib to compare with')
        self.assertEqual(tomlmini.loads_fallback(ex), tomllib.loads(ex))


if __name__ == '__main__':
    unittest.main()
