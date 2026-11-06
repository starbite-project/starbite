"""The bundle builder (sbinst/bundler.py): the per-phone deny list, the streaming writer (holes,
the zeroed store settings block, both hashes), the manifest it writes, and --from-local (hard links,
.gz sources decompressed) into a bundle that the installer's own loader and verifier accept."""
import gzip
import hashlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from sbinst import bundler, manifest as mf  # noqa: E402
from sbinst.common import MIB, PROTECTED, Refused  # noqa: E402
from sbinst.ui import UI  # noqa: E402


def quiet():
    return UI(out=io.StringIO())


class DenyList(unittest.TestCase):
    def test_sources_pass(self):
        for s in bundler.SOURCES:
            bundler.check_source(s)

    def test_per_phone_areas_refused(self):
        for name in PROTECTED + ('MISC', 'RADIO', 'A16DATA', 'ODM', 'OMR', 'SDA'):
            with self.assertRaises(Refused, msg=name):
                bundler.check_source(bundler.Src('x', 'x', 'starbite', 'part:BOOT', 'x.img', ('part', name)))

    def test_android_data_never_from_the_phone(self):
        s = next(s for s in bundler.SOURCES if s.key == 'android-data')
        self.assertIsNone(s.phone)


class Stream(unittest.TestCase):
    def test_holes_and_zeroed_settings_block(self):
        data = os.urandom(MIB) + bytes(3 * MIB) + os.urandom(12288) + os.urandom(100)
        with tempfile.TemporaryDirectory() as t:
            dest = Path(t) / 'x.img'
            chunks = (data[i:i + 70000] for i in range(0, len(data), 70000))
            raw, out = bundler.write_stream(chunks, dest, len(data), zero=bundler.SETTINGS)
            got = dest.read_bytes()
            self.assertEqual(raw, hashlib.sha256(data).hexdigest())
            want = bytearray(data)
            want[8192:12288] = bytes(4096)
            self.assertEqual(got, bytes(want))
            self.assertEqual(out, hashlib.sha256(want).hexdigest())

    def test_short_stream_stops(self):
        with tempfile.TemporaryDirectory() as t:
            with self.assertRaises(Exception):
                bundler.write_stream(iter([b'abc']), Path(t) / 'x', 4)


class FromLocal(unittest.TestCase):
    def test_build_from_local_sources(self):
        with tempfile.TemporaryDirectory() as t:
            t = Path(t)
            src = t / 'src'
            src.mkdir()
            (src / 'boot.img').write_bytes(os.urandom(8192))
            with gzip.open(src / 'a16.img.gz', 'wb') as g:
                g.write(bytes(2 * MIB) + os.urandom(4096))
            sources = t / 'sources.toml'
            sources.write_text(f'boot = "{src / "boot.img"}"\nandroid-data = "{src / "a16.img.gz"}"\n')
            out = t / 'bundle'
            os.environ['SBINST_LOCAL_ROOT'] = str(t / 'nothing-here')
            try:
                rc = bundler.build(quiet(), None, out, from_phone=False, only=['boot', 'android-data', 'android-metadata'],
                                   sources_file=sources)
            finally:
                del os.environ['SBINST_LOCAL_ROOT']
            self.assertEqual(rc, 0)
            self.assertEqual((out / 'boot' / 'starbite-boot.img').stat().st_ino, (src / 'boot.img').stat().st_ino,
                             'a plain local file on the same volume is hard-linked, not copied')
            self.assertEqual((out / 'android' / 'a16data.img').read_bytes(), bytes(2 * MIB) + gzip.decompress(
                (src / 'a16.img.gz').read_bytes())[2 * MIB:])
            m = mf.load(out)
            self.assertEqual([i.key for i in m.items], ['boot', 'android-data', 'android-metadata'])
            self.assertEqual(m.ufs_layout.name, 'starbite-a')
            self.assertEqual(mf.verify_bundle(m), [])
            # a re-run keeps finished items
            rc = bundler.build(quiet(), None, out, from_phone=False, only=['boot'], sources_file=sources)
            self.assertEqual(rc, 0)

    def test_unknown_key_refused(self):
        with tempfile.TemporaryDirectory() as t:
            with self.assertRaises(Refused):
                bundler.build(quiet(), None, Path(t) / 'b', from_phone=False, only=['efs'])


class Manifest(unittest.TestCase):
    def test_ufs_layout_in_manifest(self):
        with tempfile.TemporaryDirectory() as t:
            b = Path(t)
            (b / mf.MANIFEST_NAME).write_text('format = 1\n[ufs]\nlayout = "stock"\n')
            with self.assertRaisesRegex(Refused, 'not one of'):
                mf.load(b)
            (b / mf.MANIFEST_NAME).write_text('format = 1\n[ufs]\nlayout = "starbite-a"\n[[layout]]\nname = "WINDOWS"\n'
                                              'start = 6744832\nblocks = 1\n')
            with self.assertRaisesRegex(Refused, 'disagrees'):
                mf.load(b)
            (b / mf.MANIFEST_NAME).write_text('format = 1\n[ufs]\nlayout = "starbite-a"\n[[layout]]\nname = "A16DATA"\n'
                                              'start = 13518592\nblocks = 999\n')
            self.assertEqual(mf.load(b).ufs_layout.name, 'starbite-a')       # the filling row: start only


if __name__ == '__main__':
    unittest.main()
