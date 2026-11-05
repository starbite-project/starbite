"""Resume logic: the journal's item identity, chunk state tied to content, the chunk writer
resuming after a failure, and the plan's classification (write / skip / keep / resume / replace)."""
import hashlib
import io
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from sbinst import device, steps  # noqa: E402
from sbinst.common import CHUNK, Stopped  # noqa: E402
from sbinst.journal import Journal  # noqa: E402
from sbinst.manifest import Dest, Item  # noqa: E402
from sbinst.ui import UI  # noqa: E402


def mk_item(key='boot', dest=Dest('part', 'BOOT'), sha='a' * 64, size=4096, data=False):
    return Item(key=key, title=key, os='starbite', dest=dest, file=f'{key}.img', size=size, sha256=sha, data=data,
                present=True)


class JournalTests(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory()
        self.j = Journal(Path(self.t.name) / 'p' / 'journal.json')

    def tearDown(self):
        self.t.cleanup()

    def test_done_is_content_addressed(self):
        it = mk_item()
        self.assertFalse(self.j.item_done(it))
        self.j.mark_item(it, 'done')
        self.assertTrue(Journal(self.j.path).item_done(it))          # survives a reload
        self.assertFalse(self.j.item_done(mk_item(sha='b' * 64)))    # a new version of the image is not done
        self.assertFalse(self.j.item_done(mk_item(dest=Dest('part', 'RECOVERY'))))
        self.assertTrue(self.j.wrote_dest_before(mk_item(sha='b' * 64)))

    def test_started_is_not_done(self):
        it = mk_item()
        self.j.mark_item(it, 'started')
        self.assertFalse(self.j.item_done(it))

    def test_chunk_state_tied_to_content(self):
        self.assertNotEqual(self.j.chunk_state(mk_item()), self.j.chunk_state(mk_item(sha='b' * 64)))

    def test_dry_run_gate(self):
        self.assertFalse(self.j.had_dry_run('m1'))
        self.j.record_dry_run('m1')
        self.assertTrue(Journal(self.j.path).had_dry_run('m1'))
        self.assertFalse(self.j.had_dry_run('m2'))


class MemPhone:
    """A phone whose one partition is a bytearray; understands the chunk writer's commands."""

    def __init__(self, size, fail_on_write=None, mode='rehearse'):
        self.disk = bytearray(os.urandom(size))
        self.pushed = b''
        self.writes = 0
        self.fail_on_write = fail_on_write
        self.r = types.SimpleNamespace(mode=mode)

    def push(self, local, remote):
        import gzip
        self.pushed = gzip.decompress(Path(local).read_bytes())

    def sh(self, cmd, write=False, quiet=False, check=True):
        import re
        if write:
            self.writes += 1
            if self.fail_on_write is not None and self.writes == self.fail_on_write:
                raise Stopped('simulated cable pull')
            seek = int(re.search(r'seek=(\d+)', cmd).group(1)) << 20
            if '/dev/zero' in cmd:
                n = int(re.search(r'head -c (\d+)', cmd).group(1))
                self.disk[seek:seek + n] = bytes(n)
            else:
                self.disk[seek:seek + len(self.pushed)] = self.pushed
            return ''
        skip = int(re.search(r'skip=(\d+)', cmd).group(1)) << 20
        n = int(re.search(r'head -c (\d+)', cmd).group(1))
        return hashlib.sha256(bytes(self.disk[skip:skip + n])).hexdigest() + '  -'


class ChunkWriter(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.TemporaryDirectory()
        self.img = Path(self.t.name) / 'img'
        data = os.urandom(CHUNK) + bytes(CHUNK) + os.urandom(CHUNK // 2)      # data, zero, half chunk
        self.img.write_bytes(data)
        self.data = data
        self.state = str(Path(self.t.name) / 'chunks' / 'x.state')
        self.ui = UI(out=io.StringIO())

    def tearDown(self):
        self.t.cleanup()

    def test_interrupted_write_resumes(self):
        ph = MemPhone(4 * CHUNK, fail_on_write=2)
        with self.assertRaises(Stopped):
            device.chunk_write(ph, self.ui, self.img, len(self.data), '/dev/block/sdaX', 0, self.state, 'X')
        self.assertEqual(Path(self.state).read_text().split(), ['0'])          # chunk 0 finished, chunk 1 failed
        ph.fail_on_write, before = None, ph.writes
        device.chunk_write(ph, self.ui, self.img, len(self.data), '/dev/block/sdaX', 0, self.state, 'X')
        self.assertEqual(ph.writes - before, 2)                             # only chunks 1 and 2 again
        self.assertEqual(bytes(ph.disk[:len(self.data)]), self.data)       # zero chunk cleared stale data

    def test_offset_must_be_aligned(self):
        with self.assertRaises(Stopped):
            device.chunk_write(MemPhone(CHUNK), self.ui, self.img, 10, 'n', 4096, self.state, 'X')

    def test_dry_run_writes_nothing(self):
        ph = MemPhone(4 * CHUNK, mode='dry-run')
        before = bytes(ph.disk)
        device.chunk_write(ph, self.ui, self.img, len(self.data), 'n', 0, self.state, 'X')
        self.assertEqual(bytes(ph.disk), before)
        self.assertFalse(os.path.exists(self.state))


class Classify(unittest.TestCase):
    """steps.Installer.classify with the phone probes stubbed."""

    def setUp(self):
        self.t = tempfile.TemporaryDirectory()
        inst = steps.Installer.__new__(steps.Installer)
        inst.journal = Journal(Path(self.t.name) / 'journal.json')
        inst.o = types.SimpleNamespace(replace=[], replace_all=False)
        inst._card_status = 'skip'
        self.probe_result, self.match = 'differs', False
        inst.probe = lambda i: self.probe_result
        inst.matches = lambda i: self.match
        self.inst = inst

    def tearDown(self):
        self.t.cleanup()

    def c(self, it):
        return self.inst.classify(it)[0]

    def test_not_in_bundle(self):
        it = mk_item()
        it.present = False
        self.assertEqual(self.c(it), 'skip')

    def test_blank_target_is_written(self):
        self.probe_result = 'blank'
        self.assertEqual(self.c(mk_item(data=True)), 'write')

    def test_identical_target_is_skipped(self):
        self.match = True
        self.assertEqual(self.c(mk_item()), 'skip')

    def test_data_is_kept_unless_replace(self):
        self.probe_result = 'data'
        it = mk_item('pmos', Dest('card-file', '/distros/pmos.img'), data=True)
        self.assertEqual(self.c(it), 'keep')
        self.inst.o.replace = ['pmos']
        self.assertEqual(self.c(it), 'replace')
        self.inst.o.replace, self.inst.o.replace_all = [], True
        self.assertEqual(self.c(it), 'replace')

    def test_no_user_data_is_overwritten(self):
        self.assertEqual(self.c(mk_item('sys', Dest('part', 'SYSTEM'))), 'write')

    def test_done_data_item_is_kept_without_reading(self):
        it = mk_item('mac', Dest('card-lba', '', 216442880), data=True)
        self.inst.journal.mark_item(it, 'done')
        self.inst.probe = None                       # must not be called
        self.assertEqual(self.c(it), 'keep')

    def test_done_item_is_reverified(self):
        it = mk_item()
        self.inst.journal.mark_item(it, 'done')
        self.match = True
        self.assertEqual(self.c(it), 'skip')
        self.match = False
        self.assertEqual(self.c(it), 'write')

    def test_interrupted_item_resumes(self):
        it = mk_item('win', Dest('part', 'WINDOWS'), data=True)
        p = self.inst.journal.chunk_state(it)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text('0\n1\n')
        self.assertEqual(self.c(it), 'resume')

    def test_card_items_follow_a_new_card_layout(self):
        self.inst._card_status = 'write'
        self.probe_result = 'data'
        self.assertEqual(self.c(mk_item('pmos', Dest('card-file', '/distros/pmos.img'), data=True)), 'write')
        self.assertEqual(self.c(mk_item('st', Dest('store', 'ufs'))), 'write')   # UFS store: not on the card
        self.probe_result = 'differs'


class Mbr(unittest.TestCase):
    def test_round_trip(self):
        p1 = {'type': 7, 'start': 8192, 'sectors': 182878208}
        p2 = {'type': 0xDA, 'start': 182886400, 'sectors': 67112960}
        m = steps.build_mbr(p1, p2)
        self.assertEqual(len(m), 512)
        self.assertEqual(steps.mbr_parts(m)[:2], [(7, 8192, 182878208), (0xDA, 182886400, 67112960)])
        self.assertIsNone(steps.mbr_parts(bytes(512)))


if __name__ == '__main__':
    unittest.main()
