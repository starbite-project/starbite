"""One UI 8 on the starbite-oneui UFS layout (docs/INSTALLER.md 14): the manifest's mkfs-oneui-data action and
its ODM rule, the install order, and the A16DATA step (plan/classify, the dry-run command, the real step and
its verification) against a phone held in memory that answers the installer's adb commands."""
import hashlib
import io
import os
import re
import shutil
import sys
import tempfile
import types
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from sbinst import device, gpt, steps, tomlmini, ufslayout  # noqa: E402
from sbinst import manifest as mf  # noqa: E402
from sbinst.common import INSTALLER_DIR, ONEUI_DATA_RESERVE, Refused, Stopped  # noqa: E402
from sbinst.journal import Journal  # noqa: E402
from sbinst.ui import UI  # noqa: E402

EXAMPLE = INSTALLER_DIR / 'installer-manifest.oneui.example.toml'
ZERO = '0' * 64
L = ufslayout.get('starbite-oneui')
ROWS = {p.name: (i, s, e) for i, p, s, e in L.resolve(6, 15616000 - 6)}
A16_BLOCKS = ROWS['A16DATA'][2] - ROWS['A16DATA'][1] + 1          # 2825467 on a 15616000-block UFS
A16_FS = A16_BLOCKS - ONEUI_DATA_RESERVE                           # 2825462, as on S9 #1 (docs/ANDROID-ONEUI.md 4)
ONEUI_ITEMS = '''
[[item]]
key = "oneui-system"
title = "One UI 8 system (EROFS)"
os = "android"
file = "android/oneui-system.img"
size = 8192
sha256 = "%(z)s"
dest = "part:SYSTEM"

[[item]]
key = "oneui-vendor"
title = "One UI 8 vendor (EROFS, post-fixed)"
os = "android"
file = "android/oneui-vendor.img"
size = 8192
sha256 = "%(z)s"
dest = "part:VENDOR"

[[item]]
key = "oneui-odm"
title = "One UI 8 odm (EROFS)"
os = "android"
file = "android/oneui-odm.img"
size = 36864
sha256 = "%(z)s"
dest = "part:ODM"

[[item]]
key = "oneui-data"
title = "One UI 8 /data"
os = "android"
dest = "part:A16DATA"
action = "mkfs-oneui-data"
''' % {'z': ZERO}


def item(key, dest, extra='', file=None):
    f = f'file = "{file}"\nsize = 4096\nsha256 = "{ZERO}"\n' if file else ''
    return f'[[item]]\nkey = "{key}"\ndest = "{dest}"\n{f}{extra}\n'


class Manifest(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory()
        self.b = Path(self.t.name)

    def tearDown(self):
        self.t.cleanup()

    def load(self, body, layout='starbite-oneui'):
        head = 'format = 1\n' + (f'[ufs]\nlayout = "{layout}"\n' if layout else '')
        (self.b / mf.MANIFEST_NAME).write_text(head + body)
        return mf.load(self.b)

    def test_mkfs_oneui_data_accepted(self):
        m = self.load(ONEUI_ITEMS)
        it = m.item('oneui-data')
        self.assertEqual((it.action, str(it.dest), it.file, it.size), ('mkfs-oneui-data', 'part:A16DATA', '', 0))
        self.assertTrue(it.data, 'One UI /data is user data: never replaced without --replace')
        self.assertEqual(mf.writer_of(it), 'mke2fs on the phone (installer)')
        self.assertEqual(mf.writer_of(m.item('oneui-odm')), 'tools/android/pixel/part-write.py')
        mf.verify_bundle(m)
        self.assertTrue(it.present, 'no file: always present')
        # data = false in the manifest does not make it replaceable
        m = self.load(item('d', 'part:A16DATA', 'action = "mkfs-oneui-data"\ndata = false'))
        self.assertTrue(m.item('d').data)

    def test_mkfs_oneui_data_only_on_a16data(self):
        for dest in ('part:SYSTEM', 'part:VENDOR', 'part:ODM', 'part:WINDOWS', 'slot:USERDATA@0', 'store:ufs'):
            with self.assertRaisesRegex(Refused, 'mkfs-oneui-data only applies to part:A16DATA', msg=dest):
                self.load(item('d', dest, 'action = "mkfs-oneui-data"'))

    def test_mkfs_takes_no_file(self):
        with self.assertRaisesRegex(Refused, 'takes no file'):
            self.load(item('d', 'part:A16DATA', 'action = "mkfs-oneui-data"', file='data.img'))
        with self.assertRaisesRegex(Refused, 'takes no file'):
            self.load(item('m', 'part:ODM', 'action = "mkfs-metadata"', file='odm.img'), layout='starbite-a')

    def test_unknown_action_refused(self):
        with self.assertRaisesRegex(Refused, 'is not one of'):
            self.load(item('d', 'part:A16DATA', 'action = "mkfs-data"'))

    def test_odm_image_only_with_the_oneui_layout(self):
        m = self.load(item('odm', 'part:ODM', file='odm.img'))
        self.assertEqual((m.item('odm').action, m.item('odm').data), ('write', False))
        for layout in ('starbite-a', 'starbite-a16g', None):
            with self.assertRaisesRegex(Refused, r'only ever made fresh.*needs \[ufs\] layout = "starbite-oneui"',
                                        msg=layout):
                self.load(item('odm', 'part:ODM', file='odm.img'), layout=layout)

    def test_mkfs_metadata_rule_kept(self):
        for layout in ('starbite-a', None):
            m = self.load(item('md', 'part:ODM', 'action = "mkfs-metadata"'), layout=layout)
            self.assertTrue(m.item('md').data)
        with self.assertRaisesRegex(Refused, 'mkfs-metadata only applies to part:ODM'):
            self.load(item('md', 'part:A16DATA', 'action = "mkfs-metadata"'), layout='starbite-a')

    def test_oneui_data_and_metadata_do_not_mix(self):
        with self.assertRaisesRegex(Refused, 'has no /metadata partition'):
            self.load(item('d', 'part:A16DATA', 'action = "mkfs-oneui-data"') +
                      item('md', 'part:ODM', 'action = "mkfs-metadata"'))

    def test_recovery_still_refused(self):
        with self.assertRaisesRegex(Refused, 'never writes RECOVERY'):
            self.load(item('r', 'part:RECOVERY', file='r.img'))


class Example(unittest.TestCase):
    def test_example_parses_and_orders(self):
        text = EXAMPLE.read_text()
        try:
            import tomllib
            self.assertEqual(tomlmini.loads_fallback(text), tomllib.loads(text))
        except ImportError:
            pass
        with tempfile.TemporaryDirectory() as t:
            shutil.copy(EXAMPLE, Path(t) / mf.MANIFEST_NAME)
            m = mf.load(t)
        self.assertEqual(m.ufs_layout.name, 'starbite-oneui')
        self.assertEqual(m.layout, [], 'no [[layout]] rows: the layout gives them')
        acts = {str(i.dest): i.action for i in m.items}
        self.assertEqual(acts['part:ODM'], 'write')
        self.assertEqual(acts['part:A16DATA'], 'mkfs-oneui-data')
        self.assertNotIn('part:RECOVERY', acts)
        self.assertEqual(m.item('oneui-odm').size, 36864)
        self.assertEqual(m.item('oneui-system').size, 6419857408)
        order = [str(i.dest) for i in sorted((i for i in m.items if i.dest.kind == 'part'), key=steps.rank)]
        self.assertEqual(order, ['part:SYSTEM', 'part:VENDOR', 'part:ODM', 'part:A16DATA', 'part:BOOT'])

    def test_lineageos_order_unchanged(self):
        """LineageOS: its /data image, then its fresh /metadata (ODM) right after it, as before."""
        mk = lambda d, a='write': types.SimpleNamespace(dest=mf.parse_dest(d), action=a)  # noqa: E731
        its = [mk('part:ODM', 'mkfs-metadata'), mk('part:A16DATA'), mk('part:VENDOR'), mk('part:SYSTEM')]
        self.assertEqual([str(i.dest) for i in sorted(its, key=steps.rank)],
                         ['part:SYSTEM', 'part:VENDOR', 'part:A16DATA', 'part:ODM'])


# ------------------------------------------------------------------------------------------------- the phone
class MemPhone:
    """The A16DATA side of a phone on the starbite-oneui layout: what blockdev, blkid, dumpe2fs/tune2fs, mke2fs,
    umount and the range hashes answer. `fs` is (label, blocks, block size) once a file system is on A16DATA."""

    def __init__(self, a16_blocks=A16_BLOCKS, has_data=False, mke2fs_ignores_count=False):
        self.size = a16_blocks * 4096
        self.fs = ('data', a16_blocks - 5, 4096) if has_data else None
        self.ignore_count = mke2fs_ignores_count
        self.cmds = []

    def answer(self, cmd):
        self.cmds.append(cmd)
        if cmd.startswith('blockdev --getsize64 '):
            return str(self.size)
        if cmd.startswith('blkid '):
            node = cmd.split()[1]
            fs = f'LABEL="{self.fs[0]}" UUID="0b1c" BLOCK_SIZE="4096" TYPE="ext4" ' if self.fs else ''
            return f'{node}: {fs}PARTLABEL="A16DATA" PARTUUID="bafcb27c"'
        if cmd.startswith('dumpe2fs -h '):
            if not self.fs:
                return ''
            return (f'Filesystem volume name:   {self.fs[0]}\nInode count:              176128\n'
                    f'Block count:              {self.fs[1]}\nReserved block count:     0\n'
                    f'Block size:               {self.fs[2]}\n')
        m = re.fullmatch(r'mke2fs -t ext4 -F -L (\S+) -b (\d+) (\S+) (\d+)', cmd)
        if m:
            self.fs = (m.group(1), self.size // 4096 if self.ignore_count else int(m.group(4)), int(m.group(2)))
            return 'Writing superblocks and filesystem accounting information: done'
        if cmd.startswith('umount '):
            return ''
        m = re.search(r'head -c (\d+) \| sha256sum', cmd)
        if m:                                       # Phone.range_sha (is_blank): zeros unless a fs or data is there
            n = int(m.group(1))
            return hashlib.sha256(bytes(n) if self.fs is None else b'data').hexdigest() + '  -'
        raise AssertionError(f'unexpected phone command: {cmd}')


class MemRunner(device.Runner):
    """device.Runner whose adb answers from a MemPhone; dry-run writes are printed by Runner itself."""

    def __init__(self, ui, mode, phone):
        super().__init__(ui, mode, env={})
        self.ph = phone

    def run(self, argv, write=False, binary=False, check=True, timeout=None, env_extra=None, quiet=False,
            stream=False):
        argv = [str(a) for a in argv]
        if write and self.mode == 'dry-run':
            return super().run(argv, write=True, quiet=quiet)
        assert argv[:2] == ['adb', 'shell'], f'only adb shell is faked here, not {argv}'
        if write:
            self.writes += 1
        return device.Result(0, self.ph.answer(argv[2]), '')


class A16Step(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory()
        b = Path(self.t.name) / 'bundle'
        b.mkdir()
        (b / mf.MANIFEST_NAME).write_text('format = 1\n[ufs]\nlayout = "starbite-oneui"\n' + ONEUI_ITEMS)
        self.m = mf.load(b)
        for it in self.m.items:
            it.present = True

    def tearDown(self):
        self.t.cleanup()

    def installer(self, mode='dry-run', mounts=(), tools=('mke2fs', 'blockdev'), **phone):
        out = io.StringIO()
        ui = UI(out=out)
        ph = MemPhone(**phone)
        inst = steps.Installer.__new__(steps.Installer)
        inst.ui, inst.m, inst.mode, inst.rp = ui, self.m, mode, None
        inst.r = MemRunner(ui, mode, ph)
        inst.phone = device.Phone(inst.r)
        parts = {n: (f'sda{i}', s * 8, (e - s + 1) * 8) for n, (i, s, e) in ROWS.items()}
        parts['A16DATA'] = ('sda28', ROWS['A16DATA'][1] * 8, ph.size // 512)
        inst.facts = device.Facts(parts=parts, tools=set(tools), mounts=list(mounts))
        inst.journal = Journal(Path(self.t.name) / 'state' / 'journal.json')
        inst.o = types.SimpleNamespace(replace=[], replace_all=False, repartition=False, fresh_backup=False)
        inst._card_status, inst.card_parts = 'skip', None
        inst.done = {'written': 0, 'skipped': 0, 'kept': 0}
        inst.out, inst.ph = out, ph
        # the One UI images "differ" from what is on the phone (range hashes never match ZERO)
        inst.matches = lambda i, _m=inst.matches: False if i.action == 'write' else _m(i)
        return inst

    def plan(self, inst):
        return {s.key: (s.status, s.reason) for s in inst.plan() if s.item is not None}

    def test_order_and_blank_a16data_is_made(self):
        inst = self.installer()
        keys = [s.key for s in inst.plan() if s.item is not None]
        self.assertEqual(keys, ['oneui-system', 'oneui-vendor', 'oneui-odm', 'oneui-data'])
        st = self.plan(inst)
        self.assertEqual(st['oneui-data'], ('write', 'target is empty'))
        self.assertEqual(st['oneui-odm'][0], 'write')

    def test_data_on_a16data_is_kept_unless_replace(self):
        inst = self.installer(has_data=True)
        self.assertEqual(self.plan(inst)['oneui-data'], ('keep', 'has data: kept (--replace oneui-data overwrites it)'))
        self.assertEqual(self.plan(inst)['oneui-odm'][0], 'write', 'One UI\'s ODM image is not tied to /data')
        inst.o.replace = ['oneui-data']
        self.assertEqual(self.plan(inst)['oneui-data'][0], 'replace')
        inst.o.replace, inst.o.replace_all = [], True
        self.assertEqual(self.plan(inst)['oneui-data'][0], 'replace')

    def test_never_matches(self):
        inst = self.installer(has_data=True)
        self.assertFalse(steps.Installer.matches(inst, self.m.item('oneui-data')))

    def test_fresh_after_repartition_is_made_without_replace(self):
        inst = self.installer(has_data=True)
        ents = [gpt.Entry(i, n, s, e, ufslayout.LINUX_DATA, ufslayout.A16DATA_GUID) for n, (i, s, e) in ROWS.items()]
        inst.rp = types.SimpleNamespace(a=types.SimpleNamespace(
            status='repartition', fresh=['SYSTEM', 'VENDOR', 'ODM', 'WINDOWS', 'A16DATA'],
            plan=types.SimpleNamespace(entries=ents)))
        inst.o.repartition = True
        st = self.plan(inst)
        self.assertEqual(st['oneui-data'], ('write', 'A16DATA is new in the repartitioned table: written fresh'))
        self.assertEqual(st['oneui-odm'][0], 'write')

    def test_done_earlier_is_kept_not_reread(self):
        inst = self.installer(has_data=True)
        inst.journal.mark_item(self.m.item('oneui-data'), 'done', fresh=True)
        n = len(inst.ph.cmds)
        self.assertEqual(inst.classify(self.m.item('oneui-data'))[0], 'keep')
        self.assertEqual(len(inst.ph.cmds), n, 'not read again')

    def test_interrupted_mkfs_resumes(self):
        inst = self.installer(has_data=True)
        inst.journal.mark_item(self.m.item('oneui-data'), 'started')
        self.assertEqual(inst.classify(self.m.item('oneui-data'))[0], 'resume')

    def test_no_a16data_partition_is_blocked(self):
        inst = self.installer()
        del inst.facts.parts['A16DATA']
        self.assertEqual(self.plan(inst)['oneui-data'], ('BLOCKED', 'the phone has no A16DATA partition'))

    def test_dry_run_prints_the_command_and_writes_nothing(self):
        inst = self.installer(has_data=True, mounts=[('/dev/block/sda28', '/data', 'ext4')])
        inst.do_item(steps.Step('oneui-data', 'One UI 8 /data', '', 'replace', item=self.m.item('oneui-data')))
        out = inst.out.getvalue()
        self.assertIn("$ adb shell 'umount /data'    [dry run: not run]", out)
        self.assertIn(f"$ adb shell 'mke2fs -t ext4 -F -L data -b 4096 /dev/block/sda28 {A16_FS}'    [dry run: not run]",
                      out)
        self.assertIn(f'ext4 "data" of {A16_FS} blocks (5 blocks = 20480 bytes left free', out)
        self.assertIn(f'verify: adb shell "blkid /dev/block/sda28" has TYPE="ext4" LABEL="data"; adb shell "dumpe2fs -h '
                      f'/dev/block/sda28" (or tune2fs -l) has Block count {A16_FS} (= {A16_BLOCKS} - 5), Block size 4096',
                      out)
        self.assertEqual(inst.r.writes, 0)
        self.assertEqual(inst.r.planned[-1], f"adb shell 'mke2fs -t ext4 -F -L data -b 4096 /dev/block/sda28 {A16_FS}'")
        self.assertFalse(any(c.startswith(('mke2fs', 'umount')) for c in inst.ph.cmds), 'nothing ran on the phone')
        self.assertEqual(inst.journal.item_rec(self.m.item('oneui-data')), None)

    def test_dry_run_takes_the_size_from_the_planned_table(self):
        """Before the GPT step the phone still has the old A16DATA (layout A: 2097403 blocks); the command is for
        the new one."""
        inst = self.installer(a16_blocks=2097403)
        ents = [gpt.Entry(i, n, s, e, ufslayout.LINUX_DATA, ufslayout.A16DATA_GUID) for n, (i, s, e) in ROWS.items()]
        inst.rp = types.SimpleNamespace(a=types.SimpleNamespace(status='repartition', fresh=['A16DATA'],
                                                                plan=types.SimpleNamespace(entries=ents)))
        inst.do_item(steps.Step('oneui-data', 'One UI 8 /data', '', 'write', item=self.m.item('oneui-data')))
        self.assertIn(f'/dev/block/sda28 {A16_FS}\'', inst.out.getvalue())

    def test_real_step_makes_and_verifies(self):
        inst = self.installer(mode='rehearse')
        inst.do_item(steps.Step('oneui-data', 'One UI 8 /data', '', 'write', item=self.m.item('oneui-data')))
        self.assertIn(f'mke2fs -t ext4 -F -L data -b 4096 /dev/block/sda28 {A16_FS}', inst.ph.cmds)
        self.assertEqual(inst.ph.fs, ('data', A16_FS, 4096))
        self.assertIn(f'oneui-data: verified on the phone (read back): TYPE ext4 LABEL data, {A16_FS} blocks of 4096',
                      inst.out.getvalue())
        rec = inst.journal.item_rec(self.m.item('oneui-data'))
        self.assertEqual((rec['status'], rec['fresh'], rec['sha256']), ('done', True, 'mkfs-oneui-data'))
        self.assertEqual(inst.r.writes, 1)

    def test_wrong_block_count_stops(self):
        """A file system that fills the partition (One UI would see a /data running into its 20480-byte tail)
        fails the check."""
        inst = self.installer(mode='rehearse', mke2fs_ignores_count=True)
        with self.assertRaisesRegex(Stopped, f'VERIFY FAILED.*{A16_BLOCKS} blocks of 4096; want ext4 "data", {A16_FS}'):
            inst.do_item(steps.Step('oneui-data', 'One UI 8 /data', '', 'write', item=self.m.item('oneui-data')))
        self.assertEqual(inst.journal.item_rec(self.m.item('oneui-data'))['status'], 'started')

    def test_no_mke2fs_on_the_phone_stops(self):
        inst = self.installer(mode='rehearse', tools=('blockdev',))
        with self.assertRaisesRegex(Stopped, 'no mke2fs'):
            inst.do_item(steps.Step('oneui-data', 'One UI 8 /data', '', 'write', item=self.m.item('oneui-data')))
        self.assertFalse(any(c.startswith('mke2fs') for c in inst.ph.cmds))


class LineageCoupling(unittest.TestCase):
    """The /data + /metadata pairing still holds for LineageOS (an A16DATA image + mkfs-metadata on ODM)."""

    def test_new_data_brings_fresh_metadata(self):
        with tempfile.TemporaryDirectory() as t:
            b = Path(t)
            (b / mf.MANIFEST_NAME).write_text(
                'format = 1\n' + item('a16', 'part:A16DATA', 'data = true', file='a16.img') +
                item('md', 'part:ODM', 'action = "mkfs-metadata"'))
            m = mf.load(b)
            for it in m.items:
                it.present = True
            inst = steps.Installer.__new__(steps.Installer)
            inst.m, inst.rp, inst.o = m, None, types.SimpleNamespace(replace=[], replace_all=False, fresh_backup=False)
            inst.journal = Journal(b / 'j.json')
            inst._card_status = 'skip'
            inst.want_parts = lambda: None
            res = {'a16': ('write', 'target is empty'), 'md': ('keep', 'has data: kept')}
            inst.classify = lambda i: res[i.key]
            st = {s.key: s.status for s in inst.plan()}
            self.assertEqual((st['a16'], st['md']), ('write', 'write'))


if __name__ == '__main__':
    unittest.main()
