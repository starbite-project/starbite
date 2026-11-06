"""SBMAINT1 containers (tools/maint/sbmaint.py, pack.py): pack/verify round trip, every corruption the
loader must catch, the two-slot choice (the reference for shim/starlte) and the update plan, the
ramoops ring decoder. With build/maint/sbmaint.img present (tools/maint/env/mk-maint.sh) the real
build is verified too.  python3 -m unittest discover -s tools/maint/tests"""
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.realpath(__file__))
MAINT = os.path.dirname(HERE)
sys.path.insert(0, MAINT)
import sbmaint as sm  # noqa: E402

REPO = os.path.realpath(os.path.join(MAINT, '..', '..'))
MAIN = REPO.split(os.sep + '.claude' + os.sep + 'worktrees' + os.sep)[0]


def rd(p):
    with open(p, 'rb') as f:
        return f.read()


def wr(p, b):
    with open(p, 'wb') as f:
        f.write(b)


def fdt(tree):
    """A flattened device tree from {'props': {name: bytes}, 'children': {name: tree}} (independent of
    sbmaint.fdt_nodes)."""
    strings, struct_ = bytearray(), bytearray()

    def soff(name):
        k = name.encode() + b'\0'
        i = strings.find(k)
        if i < 0:
            i = len(strings)
            strings.extend(k)
        return i

    def node(name, t):
        struct_.extend(struct.pack('>I', 1) + name.encode() + b'\0')
        struct_.extend(bytes(-len(struct_) % 4))
        for k, v in t.get('props', {}).items():
            struct_.extend(struct.pack('>III', 3, len(v), soff(k)) + v)
            struct_.extend(bytes(-len(struct_) % 4))
        for k, v in t.get('children', {}).items():
            node(k, v)
        struct_.extend(struct.pack('>I', 2))
    node('', tree)
    struct_.extend(struct.pack('>I', 9))
    off_rsv = 40
    off_struct = off_rsv + 16
    off_strings = off_struct + len(struct_)
    total = off_strings + len(strings)
    hdr = struct.pack('>IIIIIIIIII', 0xd00dfeed, total, off_struct, off_strings, off_rsv, 17, 16, 0,
                      len(strings), len(struct_))
    return hdr + bytes(16) + bytes(struct_) + bytes(strings)


def dtb(dr_mode='peripheral', bootargs='console=tty0 loglevel=4', initrd=True, ramoops=False):
    chosen = {'bootargs': bootargs.encode() + b'\0', 'starbite,rtc-epoch': bytes(8)}
    if initrd:
        chosen.update({'linux,initrd-start': bytes(8), 'linux,initrd-end': bytes(8)})
    rm = {'ramoops@fed10000': {'props': {'compatible': b'ramoops\0' if ramoops else b'starbite,ramoops-kept\0'}}}
    usb = {'compatible': b'snps,dwc3\0', 'dr_mode': dr_mode.encode() + b'\0'}
    if dr_mode == 'otg':
        usb['usb-role-switch'] = b''
    return fdt({'props': {'model': b'Samsung Galaxy S9 (SM-G960F)\0'},
                'children': {'chosen': {'props': chosen}, 'reserved-memory': {'children': rm},
                             'soc': {'children': {'usb@10c00000': {'props': usb}}}}})


def kernel(n=100000):
    k = bytearray(os.urandom(n))
    k[56:60] = b'ARMd'
    return bytes(k)


def container(seq=1, initrd=None, rescue=True, **kw):
    return sm.pack(kernel(), dtb(), initrd if initrd is not None else os.urandom(50000),
                   dtb('otg', 'console=tty0 loglevel=7') if rescue else b'', cmdline='s9p_decon.panel_mask=2',
                   version=f'sbmaint test {seq}', seq=seq, build_time=1793000000, **kw)


class Pack(unittest.TestCase):
    def test_round_trip(self):
        k, d, r, dr = kernel(), dtb(), os.urandom(70001), dtb('otg')
        c = sm.pack(k, d, r, dr, cmdline='a=b c', version='v1', seq=7, build_time=5)
        hd = sm.verify(c)
        self.assertEqual(hd['blobs'], {'kernel': k, 'dtb': d, 'initrd': r, 'dtb-rescue': dr})
        self.assertEqual((hd['seq'], hd['cmdline'], hd['version'], hd['build_time']), (7, 'a=b c', 'v1', 5))
        self.assertEqual(len(c), hd['total'])
        self.assertEqual(len(c) % 4096, 0)
        for o, n, crc in hd['sections'].values():
            self.assertEqual(o % 4096, 0)
            self.assertEqual(zlib.crc32(c[o:o + n]) & 0xFFFFFFFF, crc)
        self.assertEqual(hd['dtb']['dtb']['dr_mode'], 'peripheral')
        self.assertEqual(hd['dtb']['dtb-rescue']['dr_mode'], 'otg')
        self.assertTrue(hd['dtb']['dtb-rescue']['role_switch'])
        self.assertFalse(hd['dtb']['dtb']['ramoops_bound'])

    def test_layout_bytes(self):
        """The header layout docs/MAINTENANCE.md 3 gives the loader agent, byte for byte."""
        c = container(seq=3)
        self.assertEqual(c[:8], b'SBMAINT1')
        self.assertEqual(struct.unpack_from('<III', c, 8), (1, 4096, 3))
        o, n, crc, kind, z = struct.unpack_from('<QQIIQ', c, 64)
        self.assertEqual((o, kind, z), (4096, 1, 0))
        self.assertEqual(c[o + 56:o + 60], b'ARMd')
        self.assertEqual(struct.unpack_from('<I', c, 4092)[0], zlib.crc32(c[:4092]) & 0xFFFFFFFF)
        self.assertEqual(c[192:192 + 22], b's9p_decon.panel_mask=2')
        self.assertEqual(c[192 + 22], 0)

    def test_no_rescue_dtb(self):
        hd = sm.verify(container(rescue=False))
        self.assertEqual(hd['sections']['dtb-rescue'], (0, 0, 0))
        self.assertEqual(hd['blobs']['dtb-rescue'], b'')

    def test_pack_refuses(self):
        with self.assertRaisesRegex(sm.ContainerError, 'arm64 Image'):
            sm.pack(os.urandom(1000), dtb(), b'x')
        with self.assertRaisesRegex(sm.ContainerError, 'flattened'):
            sm.pack(kernel(), b'not a dtb', b'x')
        with self.assertRaisesRegex(sm.ContainerError, 'empty'):
            sm.pack(kernel(), dtb(), b'')
        with self.assertRaisesRegex(sm.ContainerError, 'cmdline'):
            sm.pack(kernel(), dtb(), b'x', cmdline='x' * 256)
        with self.assertRaisesRegex(sm.ContainerError, 'staging'):
            sm.pack(kernel(60 << 20), dtb(), os.urandom(1) * (30 << 20))

    def test_verify_catches(self):
        good = container()
        hd = sm.verify(good)

        def bad(c, msg):
            with self.assertRaisesRegex(sm.ContainerError, msg):
                sm.verify(bytes(c))
        c = bytearray(good)
        c[300] ^= 1
        bad(c, 'header CRC')
        o = hd['sections']['initrd'][0]
        c = bytearray(good)
        c[o + 10] ^= 0x80
        bad(c, 'initrd CRC')
        bad(good[:-4096], 'header says')
        c = bytearray(good)
        c[:8] = b'SBMAINT2'
        bad(c, 'magic')
        bad(good[:100], 'short header')
        # a section pointing outside the container (header CRC renewed: only the bounds check catches it)
        c = bytearray(good)
        struct.pack_into('<Q', c, 64 + 2 * 32 + 8, len(good))
        struct.pack_into('<I', c, 4092, zlib.crc32(bytes(c[:4092])) & 0xFFFFFFFF)
        bad(c, 'leaves the container')
        # total over the slot
        with self.assertRaisesRegex(sm.ContainerError, 'inside the slot'):
            sm.parse_header(good, limit=len(good) - 4096)

    def test_dtb_checks(self):
        with self.assertRaisesRegex(sm.ContainerError, 'bootargs'):
            sm.verify(sm.pack(kernel(), fdt({'children': {'chosen': {'props': {'linux,initrd-start': bytes(8)}}}}), b'x'))
        with self.assertRaisesRegex(sm.ContainerError, 'placeholders'):
            sm.verify(sm.pack(kernel(), dtb(initrd=False), b'x'))
        with self.assertRaisesRegex(sm.ContainerError, 'USB device'):
            sm.verify(sm.pack(kernel(), dtb('host'), b'x'))
        with self.assertRaisesRegex(sm.ContainerError, 'rescue mode needs USB host'):
            sm.verify(sm.pack(kernel(), dtb(), b'x', dtb('peripheral')))
        self.assertTrue(sm.dtb_facts(dtb(ramoops=True))['ramoops_bound'])

    def test_restamp(self):
        c = container(seq=1)
        d = sm.restamp(c, 9)
        self.assertEqual(sm.verify(d)['seq'], 9)
        self.assertEqual(c[4096:], d[4096:])


class Slots(unittest.TestCase):
    SIZE = 256 << 20

    def reader(self, img):
        return lambda off, n: img[off:off + n]

    def test_offsets(self):
        self.assertEqual(sm.slot_b_offset(256 << 20), 128 << 20)
        self.assertEqual(sm.slot_b_offset((160 << 20) + 12345), 80 << 20)
        self.assertEqual(sm.slots(256 << 20), {'a': (0, 128 << 20), 'b': (128 << 20, 128 << 20)})

    def test_choose(self):
        a, b = container(seq=4), container(seq=5)
        img = sm.partition_image(self.SIZE, a, b)
        found, bad = sm.choose(self.reader(img), self.SIZE)
        self.assertEqual([n for n, _ in found], ['b', 'a'])
        self.assertEqual(bad, {})
        found, _ = sm.choose(self.reader(img), self.SIZE, prev=True)
        self.assertEqual(found[0][0], 'a')
        # a tie: A first
        img = sm.partition_image(self.SIZE, container(seq=2), container(seq=2))
        self.assertEqual(sm.choose(self.reader(img), self.SIZE)[0][0][0], 'a')
        # the newer slot torn in its body: the loader falls back to the older
        img = bytearray(sm.partition_image(self.SIZE, a, b))
        img[(128 << 20) + sm.verify(b)['sections']['initrd'][0] + 5] ^= 1
        found, bad = sm.choose(self.reader(bytes(img)), self.SIZE)
        self.assertEqual([n for n, _ in found], ['a'])
        self.assertIn('CRC', bad['b'])
        # nothing valid: the store is next
        found, bad = sm.choose(self.reader(bytes(self.SIZE)), self.SIZE)
        self.assertEqual(found, [])
        self.assertEqual(set(bad), {'a', 'b'})

    def test_update_plan_never_touches_the_booting_slot(self):
        img = bytearray(sm.partition_image(self.SIZE))
        new = container(seq=1)
        for want_slot, want_seq in (('a', 1), ('b', 2), ('a', 3), ('b', 4)):
            slot, off, data = sm.plan_update(self.reader(bytes(img)), self.SIZE, new)
            self.assertEqual((slot, sm.verify(data)['seq']), (want_slot, want_seq))
            before = sm.choose(self.reader(bytes(img)), self.SIZE)[0]
            # a torn write (header invalidated, half the body): the slot that booted still boots
            torn = bytearray(img)
            torn[off:off + 4096] = bytes(4096)
            torn[off + 4096:off + len(data) // 2] = data[4096:len(data) // 2]
            after = sm.choose(self.reader(bytes(torn)), self.SIZE)[0]
            self.assertNotIn(slot, [n for n, _ in after])           # the torn slot is skipped ...
            if before:                                              # ... and the one that booted still boots
                self.assertEqual(after[0][0], before[0][0])
                self.assertNotEqual(before[0][0], slot)
            img[off:off + len(data)] = data
            self.assertEqual(sm.choose(self.reader(bytes(img)), self.SIZE)[0][0][0], want_slot)


class Ramoops(unittest.TestCase):
    def zone(self, text, cap, wrapped_at=None):
        data = bytearray(cap)
        if wrapped_at is None:
            data[:len(text)] = text
            start, size = len(text), len(text)
        else:                                   # the ring wrapped: the oldest byte is at wrapped_at
            data[wrapped_at:] = text[:cap - wrapped_at]
            data[:wrapped_at] = text[cap - wrapped_at:]
            start, size = wrapped_at, cap
        return struct.pack('<III', sm.PRZ_SIG, start, size) + bytes(data)

    def test_zones(self):
        cap = 0x4000 - 12
        pm = b''.join(b'STARBITE: line %d\n' % i for i in range(2000))[:cap]
        raw = self.zone(b'<6>dmesg text\n', cap) + self.zone(pm, cap, wrapped_at=1000)
        z = sm.ramoops_zones(raw)
        self.assertEqual(z[0][2], '<6>dmesg text\n')
        self.assertEqual(z[1][1], 0xFED14000)
        self.assertEqual(z[1][2], pm.decode())
        self.assertIsNone(sm.ramoops_zones(bytes(0x8000))[0][2])


class Cli(unittest.TestCase):
    def run_pack(self, *a, ok=True):
        p = subprocess.run([sys.executable, os.path.join(MAINT, 'pack.py'), *a], capture_output=True, text=True)
        if ok:
            self.assertEqual(p.returncode, 0, p.stderr + p.stdout)
        return p

    def test_cli(self):
        with tempfile.TemporaryDirectory() as t:
            j = lambda *p: os.path.join(t, *p)  # noqa: E731
            for n, b in (('k', kernel()), ('d', dtb()), ('r', os.urandom(9999)), ('dr', dtb('otg'))):
                wr(j(n), b)
            self.run_pack('pack', j('c.img'), '--kernel', j('k'), '--dtb', j('d'), '--initrd', j('r'),
                          '--dtb-rescue', j('dr'), '--cmdline', 'x=1', '--version', 'v', '--seq', '2')
            self.assertIn('OK', self.run_pack('verify', j('c.img')).stdout)
            self.run_pack('extract', j('c.img'), j('x'))
            self.assertEqual(rd(j('x', 'initrd')), rd(j('r')))
            self.assertEqual(rd(j('x', 'cmdline.txt')), b'x=1\n')
            self.run_pack('store-entries', j('c.img'), j('s'))
            self.assertEqual(sorted(os.listdir(j('s'))), ['maint-rescue.dtb', 'maint.dtb', 'maint.image', 'maint.initrd'])
            self.run_pack('partition', j('p.img'), '--size', str(16 << 20), '--b', j('c.img'))
            self.assertIn('starts slot b', self.run_pack('select', j('p.img')).stdout)
            c = bytearray(rd(j('c.img')))
            c[5000] ^= 1
            wr(j('bad.img'), c)
            p = self.run_pack('verify', j('bad.img'), ok=False)
            self.assertEqual(p.returncode, 1)
            self.assertIn('kernel CRC mismatch', p.stderr)


def built():
    for r in (REPO, MAIN):
        p = os.path.join(r, 'build', 'maint', 'sbmaint.img')
        if os.path.isfile(p):
            return p
    return None


@unittest.skipIf(built() is None, 'no build/maint/sbmaint.img (tools/maint/env/mk-maint.sh)')
class RealBuild(unittest.TestCase):
    def test_built_container(self):
        hd = sm.verify(rd(built()))
        self.assertTrue(hd['version'].startswith('sbmaint '))
        self.assertEqual(hd['dtb']['dtb']['dr_mode'], 'peripheral')
        self.assertFalse(hd['dtb']['dtb']['ramoops_bound'])
        self.assertIn(hd['dtb']['dtb-rescue']['dr_mode'], ('otg', 'host'))
        self.assertNotIn('ramoops', hd['dtb']['dtb']['bootargs'])
        self.assertIn('s9p_decon.panel_mask=2', hd['cmdline'])
        self.assertLessEqual(hd['total'], sm.STAGE_MAX)
        self.assertEqual(hd['dtb']['dtb']['model'], 'Samsung Galaxy S9 (SM-G960F)')


if __name__ == '__main__':
    unittest.main()
