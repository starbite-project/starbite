"""The GPT engine (sbinst/gpt.py), the layout table (sbinst/ufslayout.py), the PIT check (sbinst/pit.py)
and the repartition state machine, against S9 #1's real GPT backup (read only; looked up in
backup/20261102-185306/ of this checkout or of the main checkout, or $SBINST_GPT_BACKUP) and against
fake stock GPTs built here by a writer that is independent of the engine."""
import hashlib
import importlib.util
import os
import random
import struct
import sys
import unittest
import uuid
import zlib
from dataclasses import replace
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
from sbinst import gpt, pit, ufslayout  # noqa: E402
from sbinst.common import REPO  # noqa: E402

BS = 4096
TOTAL = 15616000


def backup_dir():
    cands = [os.environ.get('SBINST_GPT_BACKUP'), REPO / 'backup' / '20261102-185306']
    if REPO.parent.name == 'worktrees':
        cands.append(REPO.parents[2] / 'backup' / '20261102-185306')
    for c in cands:
        if c and (Path(c) / 'sda-head-4M.img').is_file():
            return Path(c)
    return None


BK = backup_dir()
need_backup = unittest.skipIf(BK is None, 'S9 #1 GPT backup (backup/20261102-185306) not on this computer')


def real():
    return (BK / 'sda-head-4M.img').read_bytes(), (BK / 'sda-tail-gpt.img').read_bytes()


# --- an independent GPT writer (not gpt.py): Samsung-style header, 128 x 128 entries ---------------
def mk_gpt(entries, total=TOTAL, first=6, last=None, hsz=512, disk=ufslayout.SAMSUNG_DISK_GUID):
    """entries: [(index, name, first, last, type, guid)] -> (head LBA0..5, tail last 5 LBAs)."""
    last = last if last is not None else total - 6
    arr = bytearray(128 * 128)
    for i, name, a, b, t, g in entries:
        o = (i - 1) * 128
        arr[o:o + 16] = uuid.UUID(t).bytes_le
        arr[o + 16:o + 32] = uuid.UUID(g).bytes_le
        struct.pack_into('<QQQ', arr, o + 32, a, b, 0)
        nm = name.encode('utf-16le')
        arr[o + 56:o + 56 + len(nm)] = nm
    ecrc = zlib.crc32(arr)

    def hdr(cur, alt, elba):
        h = bytearray(BS)
        struct.pack_into('<8sIIIIQQQQ16sQIII', h, 0, b'EFI PART', 0x10000, hsz, 0, 0, cur, alt, first, last,
                         uuid.UUID(disk).bytes_le, elba, 128, 128, ecrc)
        struct.pack_into('<I', h, 16, zlib.crc32(bytes(h[:hsz])))
        return bytes(h)
    pmbr = bytearray(BS)
    struct.pack_into('<B3sB3sII', pmbr, 446, 0, bytes(3), 0xEE, bytes(3), 1, total - 1)
    pmbr[510:512] = b'\x55\xaa'
    head = bytes(pmbr) + hdr(1, total - 1, 2) + bytes(arr)
    tail = bytes(arr) + hdr(total - 1, 1, total - 5)
    return head, tail


def stock_entries(seed=1, total=TOTAL):
    rnd = random.Random(seed)
    out = []
    for i, p, s, e in ufslayout.get('stock').resolve(6, total - 6):
        g = p.unique_guid() if p.content == ufslayout.PROTECTED else str(uuid.UUID(int=rnd.getrandbits(128), version=4))
        out.append((i, p.name, s, e, p.type, g))
    return out


class Layouts(unittest.TestCase):
    def test_every_layout_validates(self):
        for L in ufslayout.LAYOUTS.values():
            ufslayout.validate(L)
        self.assertIn(ufslayout.ACTIVE, ufslayout.TARGETS)

    def test_protected_rows_are_samsungs_and_first(self):
        for L in ufslayout.LAYOUTS.values():
            self.assertEqual(L.parts[:17], ufslayout.SAMSUNG_HEAD)
            self.assertEqual(L.parts[17].name, 'SYSTEM')
            self.assertTrue(all(p.role for p in L.parts), 'every row documents its role')

    def test_rules_refuse(self):
        a = ufslayout.get('starbite-a')
        bad = [
            replace(a, parts=a.parts[:3] + (replace(a.parts[3], start=9217),) + a.parts[4:]),   # a protected row moved
            replace(a, parts=a.parts[:17] + a.parts[18:]),                                    # SYSTEM not row 18
            replace(a, parts=a.parts[:-2] + (replace(a.parts[-2], blocks=ufslayout.FILL), a.parts[-1])),
            replace(a, parts=a.parts + (a.parts[-1],)),                                       # duplicate
            replace(a, parts=a.parts[:-1] + (replace(a.parts[-1], start=13518592 - 10),)),    # overlap
        ]
        for L in bad:
            with self.assertRaises(ufslayout.LayoutError):
                ufslayout.validate(L)

    def test_samsung_guids(self):
        self.assertEqual(ufslayout.samsung_guid('BOTA0'), '52444e41-494f-2044-424f-544130000000')
        self.assertEqual(ufslayout.SAMSUNG_DISK_GUID, '52444e41-494f-2044-4d4d-43204449534b')
        self.assertEqual(uuid.UUID(ufslayout.derived_guid('X')).version, 4)


@need_backup
class RealBackup(unittest.TestCase):
    def test_parse_real_backup(self):
        h, t = real()
        d = gpt.read(h, t, TOTAL)
        self.assertTrue(d.valid, d.problems)
        self.assertEqual(d.lba, 4096, 'the header is at byte 4096: 4 KiB logical blocks')
        hdr = d.primary.header
        self.assertEqual((hdr.hsz, hdr.n, hdr.esz, hdr.first, hdr.last, hdr.alt), (512, 128, 128, 6, 15615994, 15615999))
        self.assertEqual([e.name for e in d.table][24:], ['USERDATA', 'WINESP', 'WINDOWS', 'A16DATA'])

    def test_the_table_is_s9_1_exactly(self):
        """starbite-a planned on S9 #1 changes nothing: every row, GUID and byte is the table's."""
        h, t = real()
        p = gpt.plan(gpt.read(h, t, TOTAL), ufslayout.get('starbite-a'))
        self.assertFalse(p.changed)
        self.assertEqual(p.primary, h[BS:6 * BS])
        self.assertEqual(p.backup, t)

    def test_stock_to_starbite_gives_s9_1s_bytes(self):
        """A stock phone (S9 #1's rows 1-24, one USERDATA to the end, random GUIDs from SYSTEM on) planned
        to starbite-a gives exactly S9 #1's GPT bytes."""
        h, t = real()
        head, tail = mk_gpt(stock_entries())
        d = gpt.read(head, tail, TOTAL)
        self.assertTrue(d.valid, d.problems)
        p = gpt.plan(d, ufslayout.get('starbite-a'))
        self.assertEqual(p.primary, h[BS:6 * BS])
        self.assertEqual(p.backup, t)
        st = {r.name: r.status for r in p.rows}
        self.assertEqual(st['USERDATA'], 'resized')
        self.assertEqual([st[n] for n in ('WINESP', 'WINDOWS', 'A16DATA')], ['new'] * 3)
        self.assertEqual(st['SYSTEM'], 'changed')                 # only its GUID
        self.assertTrue(all(st[p.name] == 'same' for p in ufslayout.SAMSUNG_HEAD))
        self.assertEqual(p.fresh, ['USERDATA', 'WINESP', 'WINDOWS', 'A16DATA'])

    def test_starbite_to_alternative_table_matches_gpt_a16grow(self):
        """starbite-a -> starbite-a16g (Phase 2) equals tools/android/pixel/gpt-a16grow.py's output, which
        was cross-checked with sgdisk (gpt-crosscheck.sh)."""
        h, t = real()
        p = gpt.plan(gpt.read(h, t, TOTAL), ufslayout.get('starbite-a16g'))
        spec = importlib.util.spec_from_file_location('a16grow', REPO / 'tools' / 'android' / 'pixel' / 'gpt-a16grow.py')
        g = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(g)
        prim, tail = h[:6 * BS], t
        new_p, new_b = g.build(prim, tail, None, 11421440, False)
        self.assertEqual(p.primary, new_p[BS:])
        self.assertEqual(p.backup, new_b)
        self.assertEqual(p.fresh, ['WINDOWS', 'A16DATA'])

    def test_round_trip_crcs(self):
        h, t = real()
        d = gpt.read(h, t, TOTAL)
        prim, back = gpt.build_copies(d, d.primary.entries)
        self.assertEqual((prim, back), (h[BS:6 * BS], t))
        nd = gpt.read(h[:BS] + prim, back, TOTAL)
        self.assertTrue(nd.valid)
        self.assertEqual(zlib.crc32(nd.primary.array), nd.primary.header.ecrc)

    def test_corruptions_are_seen(self):
        h, t = real()
        for where, off in (('primary header', BS + 40), ('primary entries', 2 * BS + 200), ('backup entries', 300),
                           ('backup header', 4 * BS + 40)):
            hh, tt = bytearray(h), bytearray(t)
            (hh if where.startswith('primary') else tt)[off] ^= 1
            d = gpt.read(bytes(hh), bytes(tt), TOTAL)
            self.assertFalse(d.valid, where)
            with self.assertRaises(gpt.GPTError):
                gpt.plan(d, ufslayout.get('starbite-a'))
        d = gpt.read(h, t, TOTAL + 8)                       # the wrong disk size: LBA cross references fail
        self.assertFalse(d.valid)


class FakeStock(unittest.TestCase):
    def test_stock_plan_and_preserved_bytes(self):
        head, tail = mk_gpt(stock_entries(7))
        d = gpt.read(head, tail, TOTAL)
        p = gpt.plan(d, ufslayout.get('starbite-a'))
        self.assertEqual(p.primary[:92][:16], head[BS:BS + 16])           # signature, revision, header size
        self.assertEqual(p.primary[BS:BS + 17 * 128], head[2 * BS:2 * BS + 17 * 128])
        self.assertEqual(p.backup[:17 * 128], tail[:17 * 128])
        nd = gpt.read(head[:BS] + p.primary, p.backup, TOTAL)
        self.assertTrue(nd.valid, nd.problems)
        self.assertEqual([e.name for e in nd.table][-4:], ['USERDATA', 'WINESP', 'WINDOWS', 'A16DATA'])

    def test_bigger_ufs_fills_the_last_row(self):
        total = TOTAL + (1 << 20)
        head, tail = mk_gpt(stock_entries(3, total), total=total)
        p = gpt.plan(gpt.read(head, tail, total), ufslayout.get('starbite-a'))
        self.assertEqual(p.entries[-1].last, total - 6)
        self.assertEqual(p.backup_offset, (total - 5) * BS)

    def test_refusals(self):
        L = ufslayout.get('starbite-a')
        ents = stock_entries()
        cases = {
            'EFS moved': [e if e[1] != 'EFS' else (e[0], e[1], e[2] + 1, e[3], e[4], e[5]) for e in ents],
            'BOOT size': [e if e[1] != 'BOOT' else (e[0], e[1], e[2], e[3] - 8, e[4], e[5]) for e in ents],
            'PARAM type': [e if e[1] != 'PARAM' else e[:4] + (ufslayout.LINUX_DATA, e[5]) for e in ents],
            'KEYSTORAGE guid': [e if e[1] != 'KEYSTORAGE' else e[:5] + (str(uuid.uuid4()),) for e in ents],
            'unknown partition': ents[:-1] + [(25, 'USERDATA', 1698432, 9000000, ufslayout.MS_BASIC, str(uuid.uuid4())),
                                              (26, 'MYSTERY', 9000001, TOTAL - 6, ufslayout.MS_BASIC, str(uuid.uuid4()))],
        }
        for what, e in cases.items():
            head, tail = mk_gpt(e)
            d = gpt.read(head, tail, TOTAL)
            self.assertTrue(d.valid, what)
            with self.assertRaises(gpt.GPTError, msg=what):
                gpt.plan(d, L)
        # a KEEP row (OMR) that a layout would move is refused
        rows = list(L.parts)
        i = [p.name for p in rows].index('OMR')
        moved = replace(L, parts=tuple(rows[:i]) + (replace(rows[i], start=1684352 + 256, blocks=12800 - 256),) +
                        tuple(rows[i + 1:]))
        head, tail = mk_gpt(stock_entries())
        with self.assertRaisesRegex(gpt.GPTError, 'OMR'):
            gpt.plan(gpt.read(head, tail, TOTAL), moved)

    def test_512_byte_disk_is_refused(self):
        head, tail = mk_gpt(stock_entries())
        with self.assertRaises(gpt.GPTError):
            gpt.plan(gpt.read(head[:512] + head[BS:BS + 512], tail, TOTAL, lba=512), ufslayout.get('starbite-a'))


class Interrupted(unittest.TestCase):
    """Every state a cable pull or a power loss can leave, and what the engine does next."""

    def setUp(self):
        self.head, self.tail = mk_gpt(stock_entries())
        self.d = gpt.read(self.head, self.tail, TOTAL)
        self.p = gpt.plan(self.d, ufslayout.get('starbite-a'))
        self.op, self.ob = self.d.primary_bytes(), self.tail

    def state(self, prim, back):
        ps = gpt.classify(prim, self.op, self.p.primary, 'primary', TOTAL, BS)
        bs = gpt.classify(back, self.ob, self.p.backup, 'backup', TOTAL, BS)
        return ps, bs

    @staticmethod
    def torn(new, old, first_block_new):
        """A copy whose write stopped half way: some blocks new, the rest still old."""
        return new[:BS] + old[BS:] if first_block_new else new[:-BS] + old[-BS:]

    def test_state_table(self):
        P, B = self.p.primary, self.p.backup
        cases = [((self.op, self.ob), 'write-both'),          # nothing written
                 ((self.op, self.torn(B, self.ob, False)), 'write-both'),   # the backup write torn (power loss)
                 ((self.op, B), 'write-primary'),             # cable pull between the copies
                 ((self.torn(P, self.op, True), B), 'write-primary'),       # the primary write torn
                 ((P, B), 'done')]
        self.assertEqual(self.state(self.torn(P, self.op, True), B), ('invalid', 'new'))
        self.assertEqual(self.state(self.op, self.torn(B, self.ob, False)), ('orig', 'invalid'))
        for (pr, bk), want in cases:
            self.assertEqual(gpt.next_action(*self.state(pr, bk)), want)

    def test_kernel_view_during_the_window(self):
        """Until the primary is written, the primary is the old valid table: Linux uses it (efi.c)."""
        d = gpt.read(self.head, self.p.backup, TOTAL)
        self.assertIsNotNone(d.primary)
        self.assertEqual([e.name for e in d.primary.entries][-1], 'USERDATA')

    def test_foreign_table_refused(self):
        other_h, other_t = mk_gpt(stock_entries(99))
        ps, bs = self.state(gpt.read(other_h, other_t, TOTAL).primary_bytes(), other_t)
        self.assertEqual((ps, bs), ('other', 'other'))
        with self.assertRaises(gpt.GPTError):
            gpt.next_action(ps, bs)


class Pit(unittest.TestCase):
    def test_g960f_accepted_s9plus_refused(self):
        ok, unit, lines = pit.check(pit.build(pit.stock_entries('SM-G960F')))
        self.assertTrue(ok, lines)
        self.assertEqual(unit, 4096)
        ok, _, lines = pit.check(pit.build(pit.stock_entries('SM-G965F')))
        self.assertFalse(ok)
        self.assertTrue(any('BOOT' in ln for ln in lines))

    def test_512_byte_units_accepted(self):
        rows = [replace(e, start=e.start * 8, blocks=e.blocks * 8) for e in pit.stock_entries()]
        ok, unit, _ = pit.check(pit.build(rows))
        self.assertEqual((ok, unit), (True, 512))

    def test_parse_round_trip_and_garbage(self):
        rows = pit.stock_entries()
        tags, back = pit.parse(pit.build(rows))
        self.assertEqual(back, rows)
        self.assertEqual(tags[0], 'COM_TAR2')
        with self.assertRaises(pit.PitError):
            pit.parse(b'\0' * 64)
        with self.assertRaises(pit.PitError):
            pit.parse(pit.build(rows)[:200])


if __name__ == '__main__':
    unittest.main()
