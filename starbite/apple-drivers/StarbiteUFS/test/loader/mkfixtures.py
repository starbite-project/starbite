#!/usr/bin/env python3
# SPDX-License-Identifier: APSL-2.0
"""Fixtures for the loader-side fence test (t_loader.c), written into DIR:
  gpt-oneui.img       LU 0's first 6 blocks with S9 #1's table (tools/installer/sbinst/ufslayout.py "starbite-oneui",
                      the layout on the phone since 2026-11-07; GUIDs as the installer derives them), 15616000 blocks
  gpt-<variant>.img   the same with one defect each: badsig, nouserdata, twouserdata, overlap, pastusable, many (50)
  adt-ph.bin          a small ADT whose /arm-io/ufs carries the two placeholders exactly as mkadt-board.py emits them
  adt-none.bin        the same ADT without them (a payload from before the fence)
  adt-board.bin       (when out/starlte-iphone11-1792.adt and ref/ios-herolte exist) the real mkadt-board.py output
  expect.txt          USERDATA first/blocks/uuid, the LU size, the partition count
    python3 -I -B mkfixtures.py DIR"""
import importlib.util
import struct
import subprocess
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / 'tools/installer'))
from sbinst import ufslayout  # noqa: E402

BLOCK, TOTAL = 4096, 15616000
FIRST_USABLE, LAST_USABLE = 6, 15615994


def gpt(rows, sig=b'EFI PART'):
    """rows: [(name, first, last, guid)] -> 6 blocks: MBR, header, 4 blocks of 128 x 128-byte entries"""
    img = bytearray(6 * BLOCK)
    img[510:512] = b'\x55\xaa'
    hdr = struct.pack('<8sIIIIQQQQ16sQII', sig, 0x10000, 92, 0, 0, 1, TOTAL - 1, FIRST_USABLE, LAST_USABLE,
                      uuid.uuid4().bytes_le, 2, 128, 128)
    img[BLOCK:BLOCK + len(hdr)] = hdr
    for i, (name, first, last, guid) in enumerate(rows):
        e = struct.pack('<16s16sQQQ', uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7').bytes_le,
                        uuid.UUID(guid).bytes_le, first, last, 0) + name.encode('utf-16-le').ljust(72, b'\0')
        img[2 * BLOCK + 128 * i:2 * BLOCK + 128 * (i + 1)] = e
    return bytes(img)


def node(props, children=()):
    out = struct.pack('<II', len(props), len(children))
    for name, val in props:
        out += name.encode().ljust(32, b'\0') + struct.pack('<I', len(val)) + val + bytes(-len(val) & 3)
    return out + b''.join(children)


def mkadt_board():
    spec = importlib.util.spec_from_file_location('mkadt_board', ROOT / 'tools/board/mkadt-board.py')
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


def main():
    out = Path(sys.argv[1])
    lay = ufslayout.get('starbite-oneui')
    rows = [(p.name, s, e, p.unique_guid()) for _, p, s, e in lay.resolve(FIRST_USABLE, LAST_USABLE)]
    (out / 'gpt-oneui.img').write_bytes(gpt(rows))
    (out / 'gpt-badsig.img').write_bytes(gpt(rows, b'EFI PARX'))
    (out / 'gpt-nouserdata.img').write_bytes(gpt([r for r in rows if r[0] != 'USERDATA']))
    two = list(rows)
    a16 = next(i for i, r in enumerate(two) if r[0] == 'A16DATA')
    two[a16] = ('USERDATA',) + two[a16][1:]
    (out / 'gpt-twouserdata.img').write_bytes(gpt(two))
    ov = list(rows)
    w = next(i for i, r in enumerate(ov) if r[0] == 'WINESP')
    ov[w] = (ov[w][0], ov[w][1] - 1) + ov[w][2:]          # WINESP's first block = USERDATA's last
    (out / 'gpt-overlap.img').write_bytes(gpt(ov))
    pu = list(rows)
    pu[-1] = pu[-1][:2] + (LAST_USABLE + 1,) + pu[-1][3:]  # A16DATA into the backup GPT
    (out / 'gpt-pastusable.img').write_bytes(gpt(pu))
    many = list(rows)                     # 50 entries (more than gpt_load's GPT_MAX 48): small ones after A16DATA
    many[-1] = many[-1][:2] + (LAST_USABLE - 64,) + many[-1][3:]
    for k in range(50 - len(many)):
        f = LAST_USABLE - 63 + k * 2
        many.append((f'X{k}', f, f + 1, str(uuid.uuid4())))
    (out / 'gpt-many.img').write_bytes(gpt(many))

    m = mkadt_board()
    ph = [(k, m.fence_placeholder(k, 8 if 'allowed' in k else 48)) for k in m.FENCE]
    ufs_props = [('name', b'ufs\0'), ('compatible', b'starbite,ufs\0samsung,exynos-ufs\0'),
                 ('reg', struct.pack('<8Q', 0x1120000, 0x200, 0x1121100, 0x200, 0x1110000, 0x8000, 0x1130000, 0x100))]
    other = node([('name', b'gic\0'), ('compatible', b'ARM,gicv2\0'), ('#interrupt-cells', struct.pack('<I', 1))])
    deep = node([('name', b'sdhc\0')], [node([('name', b'ufs\0'), ('starbite,allowed-ranges', ph[0][1])])])
    def tree(extra):
        arm_io = node([('name', b'arm-io\0'), ('ranges', bytes(24))], [other, deep, node(ufs_props + extra)])
        return node([('name', b'device-tree\0'), ('compatible', b'N104AP\0starbite,s9\0')],
                    [node([('name', b'chosen\0')]), arm_io, node([('name', b'cpus\0')])])
    (out / 'adt-ph.bin').write_bytes(tree(ph))
    (out / 'adt-none.bin').write_bytes(tree([]))
    base = ROOT / 'out/starlte-iphone11-1792.adt'
    if base.exists() and (ROOT / 'ref/ios-herolte/tools/adt.py').exists():
        subprocess.run([sys.executable, '-I', '-B', str(ROOT / 'tools/board/mkadt-board.py'), str(base),
                        str(out / 'adt-board.bin')], check=True, capture_output=True)
    u = next(r for r in rows if r[0] == 'USERDATA')
    (out / 'expect.txt').write_text(f'{u[1]} {u[2] - u[1] + 1} {uuid.UUID(u[3]).hex} {TOTAL} {len(rows)}\n')


if __name__ == '__main__':
    main()
