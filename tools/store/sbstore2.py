#!/usr/bin/env python3
"""The Starbite OS store, version 2 (read by shim/starlte/store.c; layout in store.h).

  sbstore2.py build OUT.img --capacity BLOCKS [--catalog catalog.toml] [--import OLD.img|DIR]
              [NAME=FILE[:TYPE] ...]
      Packs a store: the catalog (as entry "catalog"), the entries of an older store image or a
      directory of pulled entries (tools/overlay/sbstore-pull.py), then NAME=FILE arguments
      (later ones replace earlier ones of the same name). TYPE: data image dtb initrd text xnu
      uefi catalog, else taken from the name. The settings block is left empty: store-flash.sh
      keeps the phone's.
  sbstore2.py list IMG
  sbstore2.py catalog catalog.toml OUT.sbc
  sbstore2.py settings OUT.bin [--from BLOCK.bin] [--default KEY] [--timeout S] [--next KEY]
              [--last KEY]
      A settings block (4 KiB) for block 2, e.g. to set the default entry or a one-shot next.

Layout, in 4 KiB blocks: 0 "SBSTORE1" view (entries with names < 24 below 1 GiB, for older
loaders and the iOS overlay installer), 1 "SBSTORE2" header, 2 settings, 3..6 directory (256 x 64
bytes), data from 7, every entry 4 KiB aligned. CRC32 = zlib.crc32.
"""
import argparse
import struct
import sys
import tomllib
import zlib
from pathlib import Path

BLOCK = 4096
DIR_BLOCK, DIR_BLOCKS, SETTINGS_BLOCK = 3, 4, 2
DATA_BLOCK = DIR_BLOCK + DIR_BLOCKS
MAX_ENTRIES = DIR_BLOCKS * BLOCK // 64
V1_BLOCKS = 262144

TYPES = {'data': 0, 'image': 1, 'dtb': 2, 'initrd': 3, 'text': 4, 'xnu': 5, 'uefi': 6, 'catalog': 7}
CATEGORIES = {'apple': 0, 'microsoft': 1, 'android': 2, 'linux': 3, 'starbite': 4, 'other': 5, 'lab': 6}
METHODS = {'xnu': 1, 'linux': 2, 'android': 3, 'uefi': 4, 'bare': 5}
TAGS = {'': 0, 'daily': 1, 'beta': 2, 'experimental': 3, 'lab': 4, 'stable': 5}
FLAGS = {'hidden': 1}
# The loader's font: ASCII + these glyphs at 0x80.. (tools/brand/em-font.py EXTRA, ui_glyphs.h)
GLYPHS = ['°', '·', '…', '↑', '↓', '←', '→', 'µ', '±', '×', '■', '▲', '▼', '✓']


def guess_type(name):
    for suffix, t in (('.image', 'image'), ('.kernel', 'image'), ('.dtb', 'dtb'), ('.ramdisk', 'initrd'),
                      ('.initrd', 'initrd'), ('.cmdline', 'text'), ('.xnu', 'xnu'), ('.uefi', 'uefi')):
        if name.endswith(suffix):
            return TYPES[t]
    return TYPES['catalog'] if name == 'catalog' else TYPES['data']


def encode(s, what):
    out = bytearray()
    for ch in s:
        if 32 <= ord(ch) < 127:
            out.append(ord(ch))
        elif ch in GLYPHS:
            out.append(0x80 + GLYPHS.index(ch))
        else:
            sys.exit(f'{what}: {ch!r} is not in the loader font')
    return bytes(out)


def pack_catalog(path):
    doc = tomllib.loads(Path(path).read_text())
    entries = doc.get('entry', [])
    if not entries or len(entries) > 32:
        sys.exit(f'{path}: 1..32 entries')
    pool = bytearray(b'\0')       # offset 0 = empty
    seen = set()

    def string(s, what, cap):
        if not s:
            return 0
        b = encode(s, what)
        if len(b) >= cap:
            sys.exit(f'{what}: longer than {cap - 1} bytes')
        pool.extend(b + b'\0')
        return len(pool) - len(b) - 1

    recs = []
    for i, e in enumerate(entries):
        key = e['key']
        if not key or len(key) > 23 or key in seen:
            sys.exit(f'entry {i}: key {key!r} empty, too long or repeated')
        seen.add(key)
        files = e.get('files', [])
        if not 1 <= len(files) <= 4:
            sys.exit(f'{key}: 1..4 files')
        flags = 0
        for f in e.get('flags', []):
            flags |= FLAGS[f]
        offs = [string(e['name'], f'{key} name', 32), string(e.get('version', ''), f'{key} version', 48)]
        offs += [string(f, f'{key} file', 64) for f in files] + [0] * (4 - len(files))
        offs += [string(e.get('cmdline', ''), f'{key} cmdline', 512), string(e.get('root', ''), f'{key} root', 64)]
        recs.append(struct.pack('<24sBBBBHH8I', key.encode(), CATEGORIES[e['category']], METHODS[e['method']],
                                TAGS[e.get('tag', '')], int(e.get('icon', 0)), flags, int(e.get('order', i)), *offs))
    base = 32 + 64 * len(recs)
    recs = [r[:32] + struct.pack('<8I', *[o + base if o else 0 for o in struct.unpack_from('<8I', r, 32)]) for r in recs]
    total = base + len(pool)
    return struct.pack('<8sIIIIII', b'SBCATLG1', 1, len(recs), 64, base, total, 0) + b''.join(recs) + bytes(pool)


def read_store(img):
    """Entries (name, type, blob) of a store image (v2, else the v1 view), CRC checked."""
    data = Path(img).read_bytes()
    out = []
    if data[BLOCK:BLOCK + 8] == b'SBSTORE2':
        _, _, n, cap, used, dblk, dblks, sblk, dcrc, hcrc = struct.unpack_from('<8sIIQQIIIII', data, BLOCK)
        for i in range(n):
            name, blk, size, crc, typ, _fl = struct.unpack_from('<32sQQIHH', data, dblk * BLOCK + 64 * i)
            blob = data[blk * BLOCK:blk * BLOCK + size]
            out.append((name.rstrip(b'\0').decode(), typ, blob, crc))
    elif data[:8] == b'SBSTORE1':
        _, _, n, _ = struct.unpack_from('<8sIIQ', data, 0)
        for i in range(n):
            name, off, size, crc = struct.unpack_from('<24sQQI', data, 64 + 64 * i)
            out.append((name.rstrip(b'\0').decode(), None, data[off:off + size], crc))
    else:
        sys.exit(f'{img}: not a store')
    for name, _, blob, crc in out:
        if zlib.crc32(blob) != crc:
            sys.exit(f'{img}: {name} CRC mismatch')
    return [(n, t, b) for n, t, b, _ in out]


def build(a):
    items = {}
    if a.catalog:
        items['catalog'] = (TYPES['catalog'], pack_catalog(a.catalog))
    if a.imp:
        src = Path(a.imp)
        if src.is_dir():
            for f in sorted(src.iterdir()):
                if f.is_file():
                    items.setdefault(f.name, (guess_type(f.name), f.read_bytes()))
        else:
            for name, typ, blob in read_store(src):
                items.setdefault(name, (guess_type(name) if typ is None else typ, blob))
    for arg in a.files:
        name, _, rest = arg.partition('=')
        path, _, typ = rest.partition(':')
        items[name] = (TYPES[typ] if typ else guess_type(name), Path(path).read_bytes())
    if len(items) > MAX_ENTRIES:
        sys.exit('too many entries')
    dirb, v1, data = bytearray(DIR_BLOCKS * BLOCK), bytearray(BLOCK), bytearray()
    blk, nv1 = DATA_BLOCK, 0
    rows = []
    for i, (name, (typ, blob)) in enumerate(items.items()):
        if not name or len(name) > 31:
            sys.exit(f'bad entry name {name!r}')
        crc = zlib.crc32(blob)
        struct.pack_into('<32sQQIHH8x', dirb, 64 * i, name.encode(), blk, len(blob), crc, typ, 0)
        nblk = (len(blob) + BLOCK - 1) // BLOCK
        if len(name) < 24 and blk + nblk <= V1_BLOCKS and nv1 < 63:
            struct.pack_into('<24sQQII8x', v1, 64 + 64 * nv1, name.encode(), blk * BLOCK, len(blob), crc, 0)
            nv1 += 1
        rows.append((name, typ, blk, len(blob), crc))
        data += blob + bytes(-len(blob) % BLOCK)
        blk += nblk
    if blk > a.capacity:
        sys.exit(f'store needs {blk} blocks > capacity {a.capacity}')
    struct.pack_into('<8sIIQ', v1, 0, b'SBSTORE1', 1, nv1, min(blk, V1_BLOCKS))
    n = len(items)
    hdr = bytearray(BLOCK)
    struct.pack_into('<8sIIQQIIII', hdr, 0, b'SBSTORE2', 2, n, a.capacity, blk, DIR_BLOCK, DIR_BLOCKS,
                     SETTINGS_BLOCK, zlib.crc32(bytes(dirb[:64 * n])))
    struct.pack_into('<I', hdr, 48, zlib.crc32(bytes(hdr[:48])))
    Path(a.out).write_bytes(bytes(v1) + bytes(hdr) + bytes(BLOCK) + bytes(dirb) + bytes(data))
    names = {v: k for k, v in TYPES.items()}
    for name, typ, b, size, crc in rows:
        print(f'  {name:<24} {names[typ]:<8} block {b:>7} size {size:>10} crc32 {crc:08x}')
    print(f'{a.out}: {blk} blocks ({blk * BLOCK / 2**20:.1f} MiB) of {a.capacity}, {n} entries ({nv1} in the v1 view)')


def listing(a):
    names = {v: k for k, v in TYPES.items()}
    for name, typ, blob in read_store(a.img):
        print(f'  {name:<24} {names.get(typ, "?") if typ is not None else "v1":<8} {len(blob):>10} crc32 {zlib.crc32(blob):08x}')


def settings(a):
    seq, timeout, flags, keys = 0, 0, 0, ['', '', '']
    if a.frm:
        b = Path(a.frm).read_bytes()[:BLOCK]
        if b[:8] == b'SBSETTNG' and zlib.crc32(b[16:256]) == struct.unpack_from('<I', b, 12)[0]:
            seq, timeout, flags = struct.unpack_from('<III', b, 16)
            keys = [b[o:o + 24].split(b'\0')[0].decode() for o in (32, 56, 80)]
    if a.default is not None:
        keys[0] = a.default
    if a.last is not None:
        keys[1] = a.last
    if a.next is not None:
        keys[2] = a.next
    if a.timeout is not None:
        timeout = a.timeout
    if any(len(k) > 23 for k in keys):
        sys.exit('keys are at most 23 characters')
    b = bytearray(BLOCK)
    struct.pack_into('<8sII', b, 0, b'SBSETTNG', 1, 0)
    struct.pack_into('<IIII24s24s24s', b, 16, seq + 1, timeout, flags, 0, *(k.encode() for k in keys))
    struct.pack_into('<I', b, 12, zlib.crc32(bytes(b[16:256])))
    Path(a.out).write_bytes(bytes(b))
    print(f'{a.out}: default {keys[0] or "-"}, last {keys[1] or "-"}, next {keys[2] or "-"}, timeout {timeout} s')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('out')
    b.add_argument('--capacity', type=int, required=True)
    b.add_argument('--catalog')
    b.add_argument('--import', dest='imp')
    b.add_argument('files', nargs='*')
    l = sub.add_parser('list')
    l.add_argument('img')
    c = sub.add_parser('catalog')
    c.add_argument('toml')
    c.add_argument('out')
    s = sub.add_parser('settings')
    s.add_argument('out')
    s.add_argument('--from', dest='frm')
    s.add_argument('--default')
    s.add_argument('--last')
    s.add_argument('--next')
    s.add_argument('--timeout', type=int)
    a = p.parse_args()
    if a.cmd == 'build':
        build(a)
    elif a.cmd == 'list':
        listing(a)
    elif a.cmd == 'catalog':
        Path(a.out).write_bytes(pack_catalog(a.toml))
    else:
        settings(a)


if __name__ == '__main__':
    main()
