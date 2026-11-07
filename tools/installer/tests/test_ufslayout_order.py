"""ufslayout.validate: GPT entries may be out of block order (starbite-oneui moves VENDOR/ODM behind WINDOWS
without renumbering), but never overlap, and a row out of order needs an explicit start."""
import os
import sys
import unittest
from dataclasses import replace

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from sbinst import ufslayout  # noqa: E402


class OrderTest(unittest.TestCase):
    def test_oneui_out_of_order_is_valid(self):
        L = ufslayout.get('starbite-oneui')
        rows = ufslayout.validate(L, 6, L.min_blocks - 6)
        by = {p.name: (i, s, e) for i, p, s, e in rows}
        self.assertEqual(by['VENDOR'][0], 19)
        self.assertEqual(by['ODM'][0], 20)
        self.assertEqual(by['USERDATA'][0], 25)
        self.assertGreater(by['VENDOR'][1], by['WINDOWS'][2])
        self.assertEqual(by['A16DATA'][1], by['ODM'][2] + 1)
        self.assertEqual(by['SYSTEM'][2] + 1, by['CACHE'][1])

    def test_overlap_out_of_order_refused(self):
        L = ufslayout.get('starbite-oneui')
        rows = list(L.parts)
        rows[18] = replace(rows[18], start=rows[26].start + 100)        # VENDOR inside WINDOWS
        with self.assertRaises(ufslayout.LayoutError):
            ufslayout.validate(replace(L, parts=tuple(rows)), 6, L.min_blocks - 6)

    def test_implicit_start_after_moved_rows_refused(self):
        L = ufslayout.get('starbite-oneui')
        rows = list(L.parts)
        rows[20] = replace(rows[20], start=None)    # CACHE would follow ODM (behind WINDOWS): into A16DATA
        with self.assertRaises(ufslayout.LayoutError):
            ufslayout.validate(replace(L, parts=tuple(rows)), 6, L.min_blocks - 6)

if __name__ == '__main__':
    unittest.main()
