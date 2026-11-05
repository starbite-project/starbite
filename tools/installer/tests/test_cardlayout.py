"""The microSD card layouts (tools/store/cardlayout.py) and everything that takes its geometry from
them: sbstore2.py --medium sd/sd64, card-range-flash.py --layout (against a small fake card through
the fake adb), the installer manifest's [card] layout / card:root:KEY, and the bundle's layout check
(catalog roots and the sdroot= packed in SWEETXNU payloads)."""
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from sbinst import manifest as mf  # noqa: E402
from sbinst import steps, tomlmini  # noqa: E402
from sbinst.common import INSTALLER_DIR, REPO, Refused, cardlayout as cl  # noqa: E402

STORE = REPO / 'tools' / 'store'
SANDISK64 = 124735488             # a SanDisk card sold as 64 GB
CARD32 = 62521344                 # a card sold as 32 GB


def load_path(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def fake_payload(line, size=8192):
    blob = bytearray(os.urandom(size))
    blob[0:12] = cl.PAYLOAD_MAGIC + (1).to_bytes(4, 'little')
    blob[cl.PAYLOAD_CMDLINE:cl.PAYLOAD_CMDLINE + cl.PAYLOAD_CMDLINE_LEN] = line.encode().ljust(cl.PAYLOAD_CMDLINE_LEN, b'\0')
    return bytes(blob)


MAC_LINE = ('debug=0xc serial=3 cpus=8 -v -noprogress rd=sdroot0 sweetfb_gpu=1 sdroot=lba:216442880,31457280 '
            'sweetreboot=86400 sdrw=1 msgbuf=8388608 sdlog=1')


class Table(unittest.TestCase):
    def test_rules_every_layout_keeps(self):
        for L in cl.LAYOUTS.values():
            card = L.exact or L.min_sectors
            (t1, s1, c1), (t2, s2, c2) = L.parts(card)
            self.assertEqual((t1, t2), (0x07, 0xDA), L.name)
            for v in (s1, c1, s2, c2):
                self.assertEqual(v % cl.MIB_SECTORS, 0, f'{L.name}: {v} not 1 MiB aligned')
            self.assertFalse(s1 < s2 + c2 and s2 < s1 + c1, f'{L.name}: p1 and p2 overlap')
            self.assertLessEqual(max(s1 + c1, s2 + c2), card, L.name)
            pos = s2
            for a in L.areas:                                    # the areas tile p2, in order
                self.assertEqual(a.start, pos, f'{L.name} {a.name}')
                self.assertEqual(a.sectors % cl.MIB_SECTORS, 0)
                pos = a.end
            self.assertEqual(pos, s2 + c2, f'{L.name}: areas do not end at p2\'s end')
            self.assertEqual((L.areas[0].kind, L.areas[0].start), ('store', s2))
            self.assertEqual((L.tail.start, L.tail.sectors), (s2 + c2 - cl.TAIL_SECTORS, cl.TAIL_SECTORS))
            self.assertEqual(L.clock_sector, s2 + c2 - 1)
            self.assertTrue(L.root('mac13'), f'{L.name}: macOS Ventura (the active stable pair) has a root')

    def test_sd128_is_s9_1s_card_unchanged(self):
        L = cl.get('sd128')
        self.assertEqual(L.parts(249999360), [(0x07, 8192, 182878208), (0xDA, 182886400, 67112960)])
        self.assertEqual((L.root('mac13').start, L.root('mac13').sectors), (216442880, 31457280))
        self.assertEqual((L.root('ios26').start, L.root('ios26').sectors), (182888448, 33554432))
        self.assertIs(L.root('ios26sdt'), L.root('ios26'))
        self.assertEqual((L.tail.start, L.clock_sector, L.store_blocks), (249868288, 249999359, 8389120))
        self.assertEqual(L.pending, ())

    def test_the_old_constants_agree_with_the_table(self):
        """Every place that still spells the 128 GB card's numbers out agrees with sd128."""
        L = cl.get('sd128')
        sb = load_path('sbstore2_t', STORE / 'sbstore2.py')
        self.assertEqual((sb.MEDIA['sd'], sb.MEDIA['ufs'], sb.SD_ROOT_TAIL_BLOCKS), (8389120, 721024, 16384))
        crf = load_path('crf_t', REPO / 'tools' / 'macos' / 'card-range-flash.py')
        self.assertEqual((crf.CARD_SECTORS, crf.TEST_RANGE), (249999360, (249868288, 131072)))
        fp = load_path('fakephone_t', INSTALLER_DIR / 'fakes' / 'fakephone.py')
        self.assertEqual([fp.CARD_P1, fp.CARD_P2], L.parts(249999360))
        bn = (STORE / 'boot-next.sh').read_text()
        for v in (L.exact, L.tail.start, L.clock_sector):
            self.assertIn(str(v), bn)
        for patch, vals in (('0370-starlte-sdlog.patch', (L.exact, L.tail.start)),
                            ('0393-starlte-sd-clock.patch', (L.exact, L.clock_sector)),
                            ('0395-starlte-sd-clock-periodic.patch', (L.exact,))):
            text = (REPO / 'xnu' / 'series-12377' / patch).read_text()
            for v in vals:
                self.assertIn(f'{v}ULL', text, patch)
        cat = tomlmini.loads((STORE / 'catalog.toml').read_text())['entry']
        for e in cat:
            m = re.match(r'sd:lba (\d+)\+(\d+)', e.get('root', ''))
            if m:
                self.assertEqual((int(m.group(1)), int(m.group(2))), (L.root(e['key']).start, L.root(e['key']).sectors))
        m = mf.load(self._example_bundle())
        self.assertIs(m.card_layout, L)
        self.assertEqual(m.item('mac13-root').dest.offset, L.root('mac13').start)
        self.assertEqual(m.item('ios26-root').dest.offset, L.root('ios26').start)

    def _example_bundle(self):
        t = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, t)
        shutil.copy(INSTALLER_DIR / 'installer-manifest.example.toml', Path(t) / mf.MANIFEST_NAME)
        return t

    def test_sd64_on_cards_of_64_gb_and_more(self):
        L = cl.get('sd64')
        self.assertEqual(L.min_sectors, 117187500)                     # 60,000,000,000 bytes
        self.assertEqual(L.parts(117187500), [(0x07, 31727616, 85458944), (0xDA, 8192, 31719424)])
        self.assertEqual(L.parts(SANDISK64)[0], (0x07, 31727616, 93007872))
        for card in (117187500, SANDISK64, 249999360, 500170752):     # 64, 128 and 256 GB
            p1, p2 = L.parts(card)
            self.assertEqual(p2, (0xDA, 8192, 31719424), 'p2 and the roots are the same on every card')
            self.assertLessEqual(card - (p1[1] + p1[2]), cl.MIB_SECTORS - 1, 'p1 runs to the last whole MiB')
        self.assertEqual((L.root('mac13').start, L.root('mac13').sectors), (139264, 31457280))
        self.assertIsNone(L.root('ios26'))
        self.assertEqual((L.store_blocks, L.store_area_blocks, L.tail.start, L.clock_sector), (3964928, 16384, 31596544, 31727615))
        self.assertTrue(any('clock' in p for p in L.pending))
        # p1 of the smallest card holds the p1 budget of docs/STARBITE-MASTER-PLAN.md 2.4 (36 GiB)
        self.assertGreaterEqual(L.parts(117187500)[0][2] * 512, 36 << 30)

    def test_too_small_or_not_the_card(self):
        for card in (CARD32, 117187499):
            with self.assertRaises(cl.LayoutError) as e:
                cl.get('sd64').parts(card)
            self.assertIn('at least 117187500', str(e.exception))
        with self.assertRaises(cl.LayoutError) as e:
            cl.get('sd128').parts(SANDISK64)
        self.assertIn('only for the 249999360-sector card', str(e.exception))
        with self.assertRaises(cl.LayoutError):
            cl.get('sd32')

    def test_for_card_and_identify(self):
        self.assertEqual([cl.for_card(n) for n in (249999360, SANDISK64, 249999359, 500170752, CARD32)],
                         ['sd128', 'sd64', 'sd64', 'sd64', None])
        self.assertEqual(cl.identify(cl.get('sd128').parts(249999360), 249999360), 'sd128')
        self.assertEqual(cl.identify(cl.get('sd64').parts(249999360), 249999360), 'sd64')
        self.assertEqual(cl.identify(cl.get('sd64').parts(SANDISK64), SANDISK64), 'sd64')
        self.assertIsNone(cl.identify(cl.get('sd64').parts(SANDISK64), SANDISK64 + 4096))   # another card size
        self.assertIsNone(cl.identify([(0x0C, 2048, SANDISK64 - 2048)], SANDISK64))
        self.assertIsNone(cl.identify(None, SANDISK64))


class Payloads(unittest.TestCase):
    def test_cmdline(self):
        self.assertEqual(cl.retarget_cmdline(MAC_LINE, 'sd128', 'mac13'), MAC_LINE)
        new = cl.retarget_cmdline(MAC_LINE, 'sd64', 'mac13')
        self.assertEqual(new, MAC_LINE.replace('sdroot=lba:216442880,', 'sdroot=lba:139264,'))
        self.assertEqual(cl.sdroot_of(new), (139264, 31457280))
        self.assertIsNone(cl.sdroot_of('rd=md0 -v'))
        for line, key, why in ((MAC_LINE, 'ios26', 'no root on the sd64'),
                               ('rd=sdroot0 -v', 'mac13', 'no sdroot='),
                               (MAC_LINE + ' sdroot=lba:1,2', 'mac13', '2 sdroot='),
                               ('rd=sdroot0 sdroot=p2', 'mac13', 'not of the form'),
                               ('sdroot=lba:216442880,31459328', 'mac13', 'larger than'),
                               (MAC_LINE + ' x' * 250, 'mac13', 'at most 607')):
            with self.assertRaises(cl.LayoutError, msg=why) as e:
                cl.retarget_cmdline(line, 'sd64', key)
            self.assertIn(why, str(e.exception))

    def test_payload_only_the_cmdline_field_changes(self):
        blob = fake_payload(MAC_LINE)
        self.assertEqual(cl.retarget_payload(blob, 'sd128', 'mac13'), blob)     # sd128: byte-identical
        out = cl.retarget_payload(blob, 'sd64', 'mac13')
        self.assertEqual(len(out), len(blob))
        diff = [i for i in range(len(blob)) if blob[i] != out[i]]
        self.assertTrue(diff and min(diff) >= 0x48 and max(diff) < 0x48 + 608)
        self.assertEqual(cl.sdroot_arg(out), 'sdroot=lba:139264,31457280')
        with self.assertRaises(cl.LayoutError):
            cl.retarget_payload(os.urandom(4096), 'sd64', 'mac13')

    def test_cli_payload(self):
        with tempfile.TemporaryDirectory() as t:
            src, dst = Path(t, 'in.xnu'), Path(t, 'out.xnu')
            src.write_bytes(fake_payload(MAC_LINE))
            r = subprocess.run([sys.executable, '-I', str(STORE / 'cardlayout.py'), 'payload', 'sd64', 'mac13', str(src), str(dst)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('sdroot=lba:139264,31457280 (retargeted', r.stdout)
            r = subprocess.run([sys.executable, '-I', str(STORE / 'cardlayout.py'), 'payload', 'sd64', 'ios26', str(src), str(dst)],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 2)


class Catalog(unittest.TestCase):
    def test_sd128_unchanged_sd64_hides_ios26(self):
        text = (STORE / 'catalog.toml').read_text()
        self.assertEqual(cl.catalog_text(text, 'sd128'), text)
        out = cl.catalog_text(text, 'sd64')
        e = {x['key']: x for x in tomlmini.loads(out)['entry']}
        self.assertEqual(e['mac13']['root'], 'sd:lba 139264+31457280 (HFSX 15 GiB, sdrw)')
        for k in ('ios26', 'ios26sdt'):
            self.assertEqual(e[k]['flags'], ['hidden'])
            self.assertNotIn('sd:lba', e[k]['root'])
        old = {x['key']: x for x in tomlmini.loads(text)['entry']}
        self.assertEqual(sorted(e), sorted(old))                    # no key lost (sbstore2 --import refuses that)
        for k in old:
            if k not in ('mac13', 'ios26', 'ios26sdt'):
                self.assertEqual(e[k], old[k], k)
        sb = load_path('sbstore2_c', STORE / 'sbstore2.py')
        with tempfile.TemporaryDirectory() as t:
            Path(t, 'c.toml').write_text(out)
            self.assertEqual(sorted(sb.keys_of(Path(t, 'c.toml'))), sorted(old))     # packs in the loader's font


class Sbstore2(unittest.TestCase):
    def run_sb(self, *a):
        return subprocess.run([sys.executable, '-I', str(STORE / 'sbstore2.py'), *a], capture_output=True, text=True)

    def test_medium_sd64(self):
        with tempfile.TemporaryDirectory() as t:
            Path(t, 'e.img').write_bytes(os.urandom(100000))
            r = self.run_sb('build', f'{t}/s.img', '--medium', 'sd64', f'engineer.img={t}/e.img')
            self.assertEqual(r.returncode, 0, r.stderr)
            hdr = Path(t, 's.img').read_bytes()[4096:4096 + 32]
            self.assertEqual(struct.unpack_from('<8sIIQQ', hdr)[3], 3964928)
            r = self.run_sb('header', f'{t}/s.img')
            self.assertIn('sdroot=lba:31596544,131072): clear', r.stdout)
            self.assertIn('sd64 store area (p2 blocks 0..16383; the first root follows): clear', r.stdout)
            Path(t, 'big.img').write_bytes(bytes(65 << 20))
            r = self.run_sb('build', f'{t}/b.img', '--medium', 'sd64', f'big.img={t}/big.img')
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('overlap the sd64 macOS Ventura root (sdroot=lba:139264,31457280)', r.stderr)
            self.assertFalse(Path(t, 'b.img').exists())
            r = self.run_sb('build', f'{t}/b.img', '--medium', 'sd', f'big.img={t}/big.img')   # sd128: as before
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('OVERLAPPED', self.run_sb('header', f'{t}/b.img').stdout)          # ...but reported


class FakeCard:
    """The least of a fake phone card-range-flash.py needs: TWRP state and a sparse card with an MBR."""

    def __init__(self, root, sectors, parts):
        self.root = Path(root)
        (self.root / 'sys' / 'block' / 'mmcblk0').mkdir(parents=True)
        (self.root / 'dev' / 'block').mkdir(parents=True)
        (self.root / 'tmp').mkdir()
        (self.root / 'state').write_text('recovery\n')
        (self.root / 'props').write_text('')
        (self.root / 'sys' / 'block' / 'mmcblk0' / 'size').write_text(f'{sectors}\n')
        with open(self.root / 'dev' / 'block' / 'mmcblk0', 'wb') as f:
            f.truncate(sectors * 512)                       # sparse: no disk space
            f.write(steps.build_mbr(*[{'type': t, 'start': s, 'sectors': c} for t, s, c in parts]) if parts else b'')

    def flash(self, image, start, *extra):
        env = dict(os.environ, FAKE_ROOT=str(self.root),
                   PATH=f'{INSTALLER_DIR / "fakes"}{os.pathsep}{os.environ["PATH"]}')
        return subprocess.run([sys.executable, '-I', str(REPO / 'tools' / 'macos' / 'card-range-flash.py'), str(image),
                               str(start), '--check-only', *extra], capture_output=True, text=True, env=env)


class CardRangeFlash(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory()
        self.img = Path(self.t.name, 'root.img')
        self.img.write_bytes(os.urandom(1 << 20))

    def tearDown(self):
        self.t.cleanup()

    def card(self, name, sectors, layout):
        return FakeCard(Path(self.t.name, name), sectors, cl.get(layout).parts(sectors) if layout else None)

    def test_default_is_the_128_gb_card_only(self):
        c = self.card('c128', 249999360, 'sd128')
        r = c.flash(self.img, 216442880)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('sectors 216442880..216444927, inside p2', r.stdout)
        r = self.card('c64', SANDISK64, 'sd64').flash(self.img, 139264)
        self.assertIn('not the 128 GB card (249999360): refusing', r.stderr)

    def test_layout_sd64(self):
        c = self.card('c64', SANDISK64, 'sd64')
        r = c.flash(self.img, 139264, '--layout', 'sd64')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('sectors 139264..141311, inside p2', r.stdout)
        r = c.flash(self.img, 31596544, '--layout', 'sd64')            # the tail is not a root
        self.assertIn('not inside a root slot of the sd64 layout (mac13 139264+31457280): refusing', r.stderr)
        r = c.flash(self.img, 8192, '--layout', 'sd64')                # the store area neither
        self.assertIn('not inside a root slot', r.stderr)
        r = c.flash(self.img, 31727616, '--layout', 'sd64')            # p1
        self.assertIn('overlaps p1', r.stderr)
        r = c.flash(self.img, 139264, '--layout', 'sd128')
        self.assertIn('only for the 249999360-sector card', r.stderr)

    def test_layout_mismatch_and_small_card(self):
        r = self.card('c128', 249999360, 'sd128').flash(self.img, 139264, '--layout', 'sd64')
        self.assertIn('is not the sd64 layout (p1 31727616+218271744, p2 8192+31719424): it is sd128: refusing', r.stderr)
        r = self.card('blank', SANDISK64, None).flash(self.img, 139264, '--layout', 'sd64')
        self.assertIn('no MBR on the card', r.stderr)
        r = self.card('c32', CARD32, None).flash(self.img, 139264, '--layout', 'sd64')
        self.assertIn('needs a card of at least 117187500 sectors', r.stderr)


HEAD64 = '''format = 1
name = "sd64 test bundle"
catalog = "catalog.toml"
[card]
layout = "sd64"
'''


def item(key, dest, file=None, size=4096, sha='0' * 64):
    f = f'file = "{file}"\nsize = {size}\nsha256 = "{sha}"\n' if file else ''
    return f'[[item]]\nkey = "{key}"\ndest = "{dest}"\n{f}\n'


class Manifest(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory()
        self.b = Path(self.t.name)

    def tearDown(self):
        self.t.cleanup()

    def load(self, body, head=HEAD64):
        (self.b / mf.MANIFEST_NAME).write_text(head + body)
        return mf.load(self.b)

    def refused(self, body, what, head=HEAD64):
        with self.assertRaises(Refused) as e:
            self.load(body, head)
        self.assertIn(what, str(e.exception))

    def test_layout_gives_the_card(self):
        m = self.load(item('mac', 'card:root:mac13', 'm.img', 1 << 20))
        self.assertIs(m.card_layout, cl.get('sd64'))
        self.assertEqual(str(m.item('mac').dest), 'card:lba:139264')
        self.assertEqual(m.card['p2'], {'type': 0xDA, 'start': 8192, 'sectors': 31719424})
        self.assertEqual(m.card['p1'], {'type': 0x07, 'start': 31727616, 'label': 'STARBITE'})
        self.assertNotIn('sectors', m.card)
        self.assertEqual(m.card['min_bytes'], 60_000_000_000)

    def test_refusals(self):
        self.refused(item('i', 'card:root:ios26', 'i.img', 1 << 20), 'has no such root (it has: mac13)')
        self.refused(item('s', 'card:lba:8192', 's.img', 1 << 20), 'not inside a root slot of the sd64')
        self.refused(item('t', 'card:lba:31596544', 't.img', 1 << 20), 'not inside a root slot')
        self.refused(item('m', 'card:root:mac13', 'm.img', (15 << 30) + (1 << 20)), 'not inside')
        self.refused('', 'leave p1/p2 out', HEAD64 + 'p1 = { type = 0x07, start = 8192, sectors = 1000 }\n')
        self.refused('', 'fits any card', HEAD64 + 'sectors = 124735488\n')
        self.refused('', 'unknown card layout', HEAD64.replace('sd64', 'sd32'))
        self.refused(item('m', 'card:root:mac13', 'm.img', 1 << 20), 'needs [card] layout',
                     HEAD64.replace('layout = "sd64"\n', ''))
        head128 = HEAD64.replace('sd64', 'sd128')
        self.refused('', 'is not the sd128 layout', head128 + 'p2 = { type = 0xda, start = 182886400, sectors = 1000 }\n')
        self.refused('', 'is for the 249999360-sector card', head128 + 'sectors = 124735488\n')
        m = self.load(item('m', 'card:root:mac13', 'm.img', 1 << 20), head128)
        self.assertEqual((str(m.item('m').dest), m.card['sectors']), ('card:lba:216442880', 249999360))

    def bundle(self, layout, payload_layout, catalog_layout=None):
        """A bundle with a catalog and a UFS store holding the mac13 / ios26 payloads."""
        cat = cl.catalog_text((STORE / 'catalog.toml').read_text(), catalog_layout or layout)
        (self.b / 'catalog.toml').write_text(cat)
        files = {'mac13-v33.xnu': cl.retarget_payload(fake_payload(MAC_LINE), payload_layout, 'mac13'),
                 'ios26-sd.xnu': fake_payload(MAC_LINE.replace('216442880,31457280', '182888448,33554432'))}
        args = []
        for n, b in files.items():
            Path(self.t.name, n).write_bytes(b)
            args.append(f'{n}={self.t.name}/{n}')
        subprocess.run([sys.executable, '-I', str(STORE / 'sbstore2.py'), 'build', str(self.b / 'ufs.img'), '--medium', 'ufs',
                        '--catalog', str(self.b / 'catalog.toml'), *args], check=True, capture_output=True)
        head = HEAD64.replace('sd64', layout)
        m = self.load(item('ufs', 'store:ufs', 'ufs.img', (self.b / 'ufs.img').stat().st_size), head)
        m.item('ufs').path, m.item('ufs').present = self.b / 'ufs.img', True
        return mf.layout_problems(m)

    def test_bundle_matches_its_layout(self):
        self.assertEqual(self.bundle('sd128', 'sd128'), [])
        probs = self.bundle('sd64', 'sd64')
        self.assertEqual(len(probs), 1)                  # the iOS 26 payload points into sd64's p1
        self.assertIn('payload ios26-sd.xnu (ios26, hidden): sdroot=lba:182888448,33554432 is not a root slot', probs[0])

    def test_payload_or_catalog_for_another_layout(self):
        probs = self.bundle('sd64', 'sd128')
        self.assertTrue(any('payload mac13-v33.xnu (mac13): sdroot=lba:216442880,31457280; the sd64 layout puts mac13 '
                            'at 139264+31457280' in p for p in probs), probs)
        probs = self.bundle('sd64', 'sd64', catalog_layout='sd128')
        self.assertTrue(any('catalog mac13: root sd:lba 216442880+31457280, the sd64 layout has 139264' in p for p in probs), probs)
        self.assertTrue(any('catalog ios26: root sd:lba 182888448: the sd64 card layout has no root slot' in p for p in probs), probs)
        probs = self.bundle('sd128', 'sd64')
        self.assertTrue(any('the sd128 layout puts mac13 at 216442880' in p for p in probs), probs)


if __name__ == '__main__':
    unittest.main()
