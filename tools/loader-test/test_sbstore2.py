#!/usr/bin/env python3
"""tools/store/sbstore2.py against the loader's own code (run by tools/loader-test/run.sh after
host_test, which wrote OUTDIR's files with shim/starlte/sbfmt.c and catalog.c):

  - settings records: the old format (zeros from byte 104) reads as every default, round trips
    keep unknown bytes, the C encoder's record decodes the same in Python;
  - the crash log the C code wrote decodes (ring order, fields, CRCs), and `crashlog` prints it;
  - the catalog extension: what catalog.c parsed from sbstore2's catalog equals catalog_ext();
  - the decoding tables (kinds, S-Boot codes, RST_STAT bits, INFORM3, sources) are the same text;
  - build: --medium ufs adds the volatile crashlog, --import keeps its reports, the v1 view skips
    it, list/read_store do not CRC-check it.

  test_sbstore2.py OUTDIR
"""
import importlib.util
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SB = REPO / 'tools' / 'store' / 'sbstore2.py'
OUT = None


def load():
    spec = importlib.util.spec_from_file_location('sbstore2_lt', SB)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


sb = load()


def run(*a):
    return subprocess.run([sys.executable, '-I', '-B', str(SB), *map(str, a)], capture_output=True, text=True)


class Settings(unittest.TestCase):
    def test_old_record_reads_as_defaults(self):
        st = sb.settings_decode((OUT / 'set-c-old.bin').read_bytes())
        self.assertIsNotNone(st)
        self.assertEqual(st['keys'], ['linux-pmos', 'mac13', ''])
        self.assertEqual([st[f] for f in sb.SET_FIELDS] + [st['wdt_s']], [0] * 6)
        self.assertEqual(st['flags'], 1)                 # card store on; POST and log bits clear
        self.assertIn('POST full, log level quiet, crash detection on, crash screen detail 1, after a crash menu, '
                      'crash screen 60 s, watchdog per entry', sb.settings_text(st))

    def test_c_record_decodes(self):
        st = sb.settings_decode((OUT / 'set-c-new.bin').read_bytes())
        self.assertEqual((st['flags'] >> 4 & 3, st['flags'] >> 8 & 3, st['crash_mode'], st['crash_detail'],
                          st['crash_action'], st['crash_timeout'], st['wdt_mode'], st['wdt_s']), (1, 2, 1, 3, 2, 12, 2, 45))
        self.assertIn('POST quick, log level verbose', sb.settings_text(st))
        self.assertEqual(st['keys'], ['android', 'linux-ubuntu', 'rescue'])
        self.assertEqual(sb.settings_encode(st), (OUT / 'set-c-new.bin').read_bytes())

    def test_round_trip_keeps_unknown_bytes(self):
        with tempfile.TemporaryDirectory() as t:
            b = bytearray((OUT / 'set-c-old.bin').read_bytes())
            b[200] = 0x5a                                    # a field from a future tool
            struct.pack_into('<I', b, 12, zlib.crc32(bytes(b[16:256])))
            Path(t, 'in.bin').write_bytes(bytes(b))
            r = run('settings', f'{t}/out.bin', '--from', f'{t}/in.bin', '--log', 'debug')
            self.assertEqual(r.returncode, 0, r.stderr)
            o = Path(t, 'out.bin').read_bytes()
            self.assertEqual(o[200], 0x5a)
            st = sb.settings_decode(o)
            self.assertEqual((st['seq'], st['flags'], st['timeout']), (8, 1 | 3 << 8, 5))
            self.assertEqual(st['keys'], ['linux-pmos', 'mac13', ''])
            r = run('settings', f'{t}/o2.bin', '--from', f'{t}/out.bin', '--wdt', 'off', '--post', 'off',
                    '--crash-timeout', '60')
            st = sb.settings_decode(Path(t, 'o2.bin').read_bytes())
            self.assertEqual((st['wdt_mode'], st['crash_timeout'], st['flags']), (1, 0, 1 | 2 << 4 | 3 << 8))
            self.assertNotEqual(run('settings', f'{t}/x.bin', '--log', '2').returncode, 0)
            self.assertIn('watchdog off', run('settings-show', f'{t}/o2.bin').stdout)
            self.assertNotEqual(run('settings', f'{t}/x.bin', '--wdt', '90').returncode, 0)
            self.assertNotEqual(run('settings-show', f'{t}/in.bin.missing').returncode, 0)


class CrashLog(unittest.TestCase):
    def test_c_crash_log(self):
        head, reps = sb.crashlog_decode((OUT / 'crashlog.bin').read_bytes())
        self.assertEqual((head['count'], head['next'], head['launch_seq'], head['running'], head['key'], head['wdt_s']),
                         (17, 2, 18, True, 'android', 60))
        self.assertEqual([r['seq'] for r in reps], list(range(17, 2, -1)))
        r = reps[0]
        self.assertEqual((r['kind'], r['method'], r['source'], r['reason'], r['key']),
                         (3, 2, 1, 'panic: report 17', 'linux-pmos'))
        self.assertEqual(r['reg'][0], 0x11000)
        self.assertIn('Kernel panic - not syncing: test', r['text'])
        self.assertEqual(r['event'], 1794199990)

    def test_cli(self):
        r = run('crashlog', OUT / 'crashlog.bin', '--level', '3')
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('crash log: 17 reports written', r.stdout)
        self.assertIn('--- report #17: postmarketOS (linux-pmos, linux)', r.stdout)
        self.assertIn('kind: KERNEL PANIC [crash]', r.stdout)
        self.assertIn('RST_STAT', r.stdout)
        self.assertEqual(r.stdout.count('--- report #'), 15)

    def test_damaged_report(self):
        b = bytearray((OUT / 'crashlog.bin').read_bytes())
        b[4096 * 3 + 500] ^= 1
        _, reps = sb.crashlog_decode(bytes(b))
        self.assertEqual(sum(1 for r in reps if r.get('bad')), 1)

    def test_empty(self):
        head, reps = sb.crashlog_decode(bytes(sb.CL_BYTES))
        self.assertIsNone(head)


class Tables(unittest.TestCase):
    def test_same_names(self):
        c = (OUT / 'names-c.txt').read_text().splitlines()
        py = [f'kind {k} {n}' for k, n in enumerate(sb.KINDS)]
        py += [f'rr {k} {n}' for k, n in sorted(sb.RR_NAMES.items())]
        py += [f'rst {b} {n}' for b, n in sorted(sb.RST_BITS.items())]
        for v in (0, 0x12345670, 0x12345671, 0x12345672, 0x12345673, 0x12345674, 0x12345675, 0x1234567d, 0xC8, 0x66,
                  0x85, 0xCAFEBABE):
            py.append(f'inform3 {v:#x} {sb.INFORM3_NAMES[v]}')
        py += [f'source {k} {n}' for k, n in enumerate(sb.SOURCES)]
        self.assertEqual(c, py)


class Catalog(unittest.TestCase):
    def test_c_parse_equals_python(self):
        blob = (OUT / 'cat.sbc').read_bytes()
        ext = sb.catalog_ext(blob)
        keys = sb.catalog_keys(blob)
        rows = [line.split('|') for line in (OUT / 'cat-c.txt').read_text().splitlines()]
        self.assertEqual([r[0] for r in rows], keys)
        for r, (log, wdt, feed, flags) in zip(rows, ext):
            self.assertEqual((int(r[2]), int(r[3]), int(r[4])), (wdt, feed, flags), r[0])
            self.assertEqual(r[5:9], log, r[0])

    def test_defaults_and_overrides(self):
        ext = dict(zip(sb.catalog_keys((OUT / 'cat.sbc').read_bytes()), sb.catalog_ext((OUT / 'cat.sbc').read_bytes())))
        # [defaults.linux] wdt = 0 until a watchdog reset is seen to reach the loader (catalog.toml, 1f28633)
        self.assertEqual(ext['linux-pmos'][1:], (0, 1, 1))
        self.assertEqual(ext['linux-bringup'], (['', '', '', ''], 0, 1, 1))
        self.assertEqual(ext['android'][1:3], (0, 3))
        self.assertEqual(ext['lab-failtest'][3], 2)
        self.assertEqual(ext['win11'], (['', '', '', ''], 0, 0, 0))

    def test_old_catalog_has_no_extension(self):
        b = bytearray((OUT / 'cat.sbc').read_bytes())
        b[28:32] = bytes(4)
        self.assertIsNone(sb.catalog_ext(bytes(b)))
        self.assertEqual(sb.catalog_keys(bytes(b)), sb.catalog_keys((OUT / 'cat.sbc').read_bytes()))

    def test_bad_fields(self):
        with tempfile.TemporaryDirectory() as t:
            base = ('[[entry]]\nkey = "x"\nname = "X"\ncategory = "linux"\nmethod = "linux"\nfiles = ["a"]\n')
            for extra, msg in (('wdt = 100\n', 'wdt 100'), ('wdt_feed = "cron"\n', 'wdt_feed'),
                               ('log = ["a"]\n', 'log must be'), ('log = ["' + 'a' * 130 + '", "", "", ""]\n', 'longer than 127')):
                Path(t, 'c.toml').write_text(base + extra)
                r = run('catalog', f'{t}/c.toml', f'{t}/c.sbc')
                self.assertNotEqual(r.returncode, 0, extra)
                self.assertIn(msg, r.stderr)
            Path(t, 'c.toml').write_text('[defaults.lnx]\nwdt = 30\n' + base)
            self.assertIn('not a method', run('catalog', f'{t}/c.toml', f'{t}/c.sbc').stderr)


class Build(unittest.TestCase):
    def test_crashlog_entry(self):
        with tempfile.TemporaryDirectory() as t:
            Path(t, 'k').write_bytes(b'K' * 5000)
            r = run('build', f'{t}/s.img', f'linux.dtb={t}/k', '--medium', 'ufs', '--catalog', REPO / 'tools/store/catalog.toml')
            self.assertEqual(r.returncode, 0, r.stderr)
            ents = {n: (b, fl) for n, _, b, fl in sb.read_store(f'{t}/s.img', with_flags=True)}
            self.assertEqual(len(ents['crashlog'][0]), sb.CL_BYTES)
            self.assertEqual(ents['crashlog'][1], sb.SF_VOLATILE)
            self.assertEqual(ents['linux.dtb'][1], 0)
            # the loader writes into it: CRC no longer matches, the store still reads and lists
            img = bytearray(Path(t, 's.img').read_bytes())
            dirb = img[3 * 4096:4 * 4096]
            for i in range(len(ents)):
                name, blk, size = struct.unpack_from('<32sQQ', dirb, 64 * i)
                if name.rstrip(b'\0') == b'crashlog':
                    img[blk * 4096:blk * 4096 + sb.CL_BYTES] = (OUT / 'crashlog.bin').read_bytes()
                    v1 = img[:4096]
                    self.assertNotIn(b'crashlog', bytes(v1), 'the v1 view must not list the crash log')
            Path(t, 'live.img').write_bytes(bytes(img))
            self.assertIn('volatile', run('list', f'{t}/live.img').stdout)
            self.assertIn('17 reports written', run('crashlog', f'{t}/live.img').stdout)
            # a rebuild that imports the live store keeps the reports
            r = run('build', f'{t}/s2.img', '--medium', 'ufs', '--import', f'{t}/live.img')
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('17 reports written', run('crashlog', f'{t}/s2.img').stdout)
            # card stores get none
            r = run('build', f'{t}/sd.img', f'linux.dtb={t}/k', '--medium', 'sd64')
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertNotIn('crashlog', [n for n, _, _ in sb.read_store(f'{t}/sd.img')])
            r = run('build', f'{t}/s3.img', '--medium', 'ufs', '--import', f'{t}/live.img', '--no-crashlog')
            self.assertNotIn('crashlog', [n for n, _, _ in sb.read_store(f'{t}/s3.img')])


if __name__ == '__main__':
    OUT = Path(sys.argv.pop(1))
    unittest.main(verbosity=1)
