#!/usr/bin/env python3
"""The Starbite OS store, version 2 (read by shim/starlte/store.c; layout in store.h).

  sbstore2.py build OUT.img (--capacity BLOCKS | --medium ufs|sd|sd64) [--catalog catalog.toml]
              [--import OLD.img|DIR] [NAME=FILE[:TYPE] ...]
      Packs a store: the catalog (as entry "catalog"), the entries of an older store image or a
      directory of pulled entries (tools/overlay/sbstore-pull.py), then NAME=FILE arguments
      (later ones replace earlier ones of the same name). TYPE: data image dtb initrd text xnu
      uefi catalog, else taken from the name. The settings block is left empty: store-flash.sh
      keeps the phone's. --medium gives the capacity of S9 #1's store areas: ufs = USERDATA
      after 16 GiB (721024), sd = the 128 GB card's partition 2 (8389120); store-flash*.sh check
      it against the phone. --medium sd64 is the card store of the sd64 card layout (any card of
      at least 64 GB, tools/store/cardlayout.py): capacity = that layout's p2 (3964928 blocks), and the
      store must stay inside its 64 MiB store area (the macOS Ventura root follows it). The card
      geometry (sd = sd128, sd64) comes from cardlayout.py. A card store is written by store-flash-sd.sh; the loader reads it
      only with the settings flag on (settings --sd on, or Engineer "Card store at boot").
      With both --catalog and --import, refuses when the new catalog lacks a menu key the imported
      store's catalog has (another workflow's row would vanish): --drop KEY (repeatable) allows it.
      --medium ufs also refuses a store that reaches the last 1025 blocks of USERDATA (the iOS 26
      kernel's herolog ring, bsd/dev/herolte_ufs.c). --medium sd refuses a store that reaches the
      last 16384 blocks (64 MiB) of card p2: the XNU microSD root test range
      sdroot=lba:249868288,131072 (bsd/dev/starlte_sd.c, docs/XNU-MICROSD.md section 9).
  sbstore2.py list IMG
  sbstore2.py header IMG
      The version 2 header: entries, capacity and used blocks; for a card store (capacity 8389120,
      sd128; 3964928, sd64) also whether the used end clears the microSD root tail at block 8372736
      (sd64: 3948544) and the layout's store area (the first root starts after it).
  sbstore2.py catalog catalog.toml OUT.sbc
  sbstore2.py catalog-keys IMG|OUT.sbc|catalog.toml
      The menu keys of a catalog, one per line (sorted): diff two of them before flashing.
  sbstore2.py settings OUT.bin [--from BLOCK.bin] [--default KEY] [--timeout S] [--next KEY]
              [--last KEY] [--sd on|off] [--post full|quick|off] [--log quiet|normal|verbose|debug]
              [--crash on|save|off] [--crash-detail 1..3] [--crash-action menu|recovery|retry]
              [--crash-timeout S] [--wdt entry|off|SECONDS]
      A settings block (4 KiB) for block 2, e.g. to set the default entry or a one-shot next. With
      --from, every field (also ones this tool does not know, bytes 112..255) is kept unless an
      option changes it. The POST level is flags bits 4-5 (0 full, 1 quick, 2 off), the log level
      flags bits 8-9 (0 quiet, 1 normal, 2 verbose, 3 debug): one definition with the loader
      (shim/starlte/store.h) and Engineer OS. Defaults (zeros, also in records older than
      2026-11-06): full POST, quiet, crash detection on, screen detail 1, then the menu, 60 s,
      watchdog per entry.
  sbstore2.py settings-show BLOCK.bin
  sbstore2.py crashlog SRC [--level 1|2|3]
      The loader's crash reports (docs/STARBITE-HEALTH.md), newest first. SRC: a store image (its
      "crashlog" entry), the raw 64 KiB entry, or "live": the phone in the loader's fastboot
      (`fastboot oem log crashraw` + `fastboot get_staged`). Level 1 summary, 2 + the last log
      lines, 3 + registers.

The "crashlog" entry (16 blocks, directory flag 1 = volatile: written in place by the loader, so
its CRC is not checked) is added to every --medium ufs store; --import keeps the phone's reports.
The catalog gets the extension table SBCATEX1 (catalog.h): per entry the command line fragments
for log levels 0..3 (`log = [...]`), the watchdog (`wdt = SECONDS`, `wdt_feed = kernel|userspace|
own|xnu|none`) and flags (`crash_return = true`, `no_launch = true`); `[defaults.<method>]` tables
give them to every entry of that method that does not set its own.

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

sys.dont_write_bytecode = True                     # no __pycache__ in tools/store
sys.path.insert(0, str(Path(__file__).resolve().parent))
import cardlayout  # noqa: E402  (the card geometry: tools/store/cardlayout.py)

BLOCK = 4096
DIR_BLOCK, DIR_BLOCKS, SETTINGS_BLOCK = 3, 4, 2
DATA_BLOCK = DIR_BLOCK + DIR_BLOCKS
MAX_ENTRIES = DIR_BLOCKS * BLOCK // 64
V1_BLOCKS = 262144
SD_LAYOUTS = {'sd': cardlayout.get('sd128'), 'sd64': cardlayout.get('sd64')}
# S9 #1: USERDATA after 16 GiB; card p2 182886400..249999359 (sd = sd128: 8389120); sd64: p2 8192..31727615 (3964928)
MEDIA = {'ufs': 721024, **{m: L.store_blocks for m, L in SD_LAYOUTS.items()}}
HEROLOG_BLOCKS = 1025                     # herolte_ufs.c: 4 MiB ring + header block at the end of the root partition
SD_ROOT_TAIL_BLOCKS = cardlayout.TAIL_SECTORS // cardlayout.BLOCK_SECTORS   # 16384: p2's last 64 MiB, the tail
SET_SD_STORE = 1                          # settings flags: read the card's store at boot
SF_VOLATILE = 1                           # directory flags: written in place by the loader, no CRC check
CRASHLOG = 'crashlog'
VOLATILE_KNOWN = (CRASHLOG, 'sbos-state')   # entries written in place by the loader / Starbite OS
CL_BLOCK, CL_SLOTS = 4096, 15
CL_BYTES = CL_BLOCK * (1 + CL_SLOTS)
WDT_FEEDS = {'none': 0, 'kernel': 1, 'userspace': 2, 'own': 3, 'xnu': 4}
CE_LINUX_INFORM3, CE_NO_LAUNCH = 1, 2
LOG_MAX = 128                             # catalog.h CAT_LOG_MAX: a fragment is at most 127 bytes

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


def entry_ext(e, defaults):
    """(log fragments x4, watchdog s, feeder, flags) of a catalog entry: its own fields, else the
    [defaults.<method>] table's."""
    d = defaults.get(e['method'], {})

    def f(k, dv):
        return e[k] if k in e else d.get(k, dv)
    log = f('log', ['', '', '', ''])
    if not isinstance(log, list) or len(log) != 4 or not all(isinstance(x, str) for x in log):
        sys.exit(f"{e['key']}: log must be a list of 4 strings (log levels 0..3)")
    wdt = int(f('wdt', 0))
    if wdt and not 10 <= wdt <= 82:
        sys.exit(f"{e['key']}: wdt {wdt}: 0 (off) or 10..82 s (the watchdog's 16-bit counter at 793 Hz)")
    feed = f('wdt_feed', 'none')
    if feed not in WDT_FEEDS:
        sys.exit(f"{e['key']}: wdt_feed {feed!r}: one of {', '.join(WDT_FEEDS)}")
    flags = (CE_LINUX_INFORM3 if f('crash_return', False) else 0) | (CE_NO_LAUNCH if f('no_launch', False) else 0)
    return log, wdt, WDT_FEEDS[feed], flags


def pack_catalog(path):
    doc = tomllib.loads(Path(path).read_text())
    entries = doc.get('entry', [])
    defaults = doc.get('defaults', {})
    if not entries or len(entries) > 32:
        sys.exit(f'{path}: 1..32 entries')
    for m in defaults:
        if m not in METHODS:
            sys.exit(f'{path}: [defaults.{m}]: not a method ({", ".join(METHODS)})')
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

    recs, exts = [], []
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
        log, wdt, feed, cflags = entry_ext(e, defaults)
        lofs = [string(x, f'{key} log[{k}]', LOG_MAX) for k, x in enumerate(log)]
        exts.append((lofs, wdt, feed, cflags))
    # header 32 | records 64 n | SBCATEX1 24 + 32 n | strings (offsets from the catalog start)
    ext_off = 32 + 64 * len(recs)
    base = ext_off + 24 + 32 * len(exts)
    recs = [r[:32] + struct.pack('<8I', *[o + base if o else 0 for o in struct.unpack_from('<8I', r, 32)]) for r in recs]
    ext = struct.pack('<8sIIII', b'SBCATEX1', 1, len(exts), 32, 0) + b''.join(
        struct.pack('<4IHBB12x', *[o + base if o else 0 for o in lofs], wdt, feed, cflags)
        for lofs, wdt, feed, cflags in exts)
    total = base + len(pool)
    return (struct.pack('<8sIIIIII', b'SBCATLG1', 1, len(recs), 64, base, total, ext_off) + b''.join(recs) + ext
            + bytes(pool))


def catalog_ext(blob):
    """Per entry (log fragments x4, wdt, feeder, flags) from a packed catalog's SBCATEX1 table, or
    None without one (catalogs packed before 2026-11-06)."""
    _, _, n, _, _, total, ext = struct.unpack_from('<8sIIIIII', blob, 0)
    if not ext or blob[ext:ext + 8] != b'SBCATEX1':
        return None
    _, ver, cnt, esz, _ = struct.unpack_from('<8sIIII', blob, ext)
    if ver != 1 or cnt != n or esz != 32:
        return None

    def at(o):
        return blob[o:blob.index(b'\0', o)].decode() if o else ''
    out = []
    for i in range(n):
        *lofs, wdt, feed, flags = struct.unpack_from('<4IHBB', blob, ext + 24 + 32 * i)
        out.append(([at(o) for o in lofs], wdt, feed, flags))
    return out


def check_header(hdr, img='store'):
    """(n, cap, used) of a version 2 header block, or exit: magic, version, CRC, bounds."""
    magic, ver, n, cap, used, dblk, dblks, sblk, dcrc, hcrc = struct.unpack_from('<8sIIQQIIIII', hdr, 0)
    if magic != b'SBSTORE2':
        sys.exit(f'{img}: no version 2 header (magic {magic!r})')
    if zlib.crc32(bytes(hdr[:48])) != hcrc:
        sys.exit(f'{img}: header CRC mismatch')
    if ver != 2 or (dblk, dblks, sblk) != (DIR_BLOCK, DIR_BLOCKS, SETTINGS_BLOCK) or n > MAX_ENTRIES:
        sys.exit(f'{img}: unexpected header (version {ver}, {n} entries, directory {dblk}+{dblks})')
    if not DATA_BLOCK <= used <= cap:
        sys.exit(f'{img}: used {used} blocks outside [{DATA_BLOCK}, capacity {cap}]')
    return n, cap, used, dcrc


def read_store(img, with_flags=False):
    """Entries (name, type, blob) of a store image (v2, else the v1 view), CRC checked except volatile
    entries (the crash log). with_flags: (name, type, blob, flags)."""
    data = Path(img).read_bytes()
    out = []
    if data[BLOCK:BLOCK + 8] == b'SBSTORE2':
        n, cap, used, dcrc = check_header(data[BLOCK:BLOCK + 52], img)
        if len(data) < used * BLOCK:
            sys.exit(f'{img}: {len(data)} bytes, header says {used} blocks')
        dirb = data[DIR_BLOCK * BLOCK:DIR_BLOCK * BLOCK + 64 * n]
        if zlib.crc32(dirb) != dcrc:
            sys.exit(f'{img}: directory CRC mismatch')
        for i in range(n):
            name, blk, size, crc, typ, fl = struct.unpack_from('<32sQQIHH', dirb, 64 * i)
            if blk < DATA_BLOCK or blk + (size + BLOCK - 1) // BLOCK > used:
                sys.exit(f'{img}: entry {name.rstrip(bytes(1))!r} at block {blk} + {size} B is outside [{DATA_BLOCK}, {used})')
            blob = data[blk * BLOCK:blk * BLOCK + size]
            out.append((name.rstrip(b'\0').decode(), typ, blob, crc, fl))
    elif data[:8] == b'SBSTORE1':
        _, _, n, _ = struct.unpack_from('<8sIIQ', data, 0)
        for i in range(n):
            name, off, size, crc = struct.unpack_from('<24sQQI', data, 64 + 64 * i)
            out.append((name.rstrip(b'\0').decode(), None, data[off:off + size], crc, 0))
    else:
        sys.exit(f'{img}: not a store')
    for name, _, blob, crc, fl in out:
        if not fl & SF_VOLATILE and zlib.crc32(blob) != crc:
            sys.exit(f'{img}: {name} CRC mismatch')
    if with_flags:
        return [(n, t, b, fl) for n, t, b, _, fl in out]
    return [(n, t, b) for n, t, b, _, _ in out]


def catalog_keys(blob, what='catalog'):
    """The menu keys of a packed SBCATLG1 catalog."""
    magic, ver, n, recsize, base, total, _ = struct.unpack_from('<8sIIIIII', blob, 0)
    if magic != b'SBCATLG1' or recsize != 64 or 32 + 64 * n > len(blob):
        sys.exit(f'{what}: not a packed catalog (magic {magic!r})')
    return [blob[32 + 64 * i:32 + 64 * i + 24].split(b'\0')[0].decode() for i in range(n)]


def keys_of(path):
    """Catalog keys of a store image (its 'catalog' entry), a packed .sbc or a catalog TOML."""
    p = Path(path)
    if p.suffix == '.toml':
        return catalog_keys(pack_catalog(p), str(p))
    head = p.read_bytes()[:8]
    if head == b'SBCATLG1':
        return catalog_keys(p.read_bytes(), str(p))
    cat = {n: b for n, _, b in read_store(p)}.get('catalog')
    if cat is None:
        sys.exit(f'{path}: no catalog entry')
    return catalog_keys(cat, f'{path} catalog')


def build(a):
    items = {}
    vol = set()                       # volatile entries (the crash log): flag SF_VOLATILE, not in the v1 view
    if a.catalog:
        items['catalog'] = (TYPES['catalog'], pack_catalog(a.catalog))
    if a.catalog and a.imp:
        src = Path(a.imp)
        old = None
        if src.is_dir():
            if (src / 'catalog').is_file():
                old = (src / 'catalog').read_bytes()
        else:
            old = {n: b for n, _, b in read_store(src)}.get('catalog')
        if old is not None:
            lost = set(catalog_keys(old, f'{a.imp} catalog')) - set(catalog_keys(items['catalog'][1])) - set(a.drop)
            if lost:
                sys.exit(f'{a.catalog} lacks menu keys the imported catalog has: {" ".join(sorted(lost))}. '
                         'Build the catalog from the TOML the live store was built from, or pass --drop KEY '
                         'for each key you mean to remove.')
    if a.imp:
        src = Path(a.imp)
        if src.is_dir():
            for f in sorted(src.iterdir()):
                if f.is_file():
                    items.setdefault(f.name, (guess_type(f.name), f.read_bytes()))
                    if f.name in VOLATILE_KNOWN:
                        vol.add(f.name)
        else:
            for name, typ, blob, fl in read_store(src, with_flags=True):
                items.setdefault(name, (guess_type(name) if typ is None else typ, blob))
                if fl & SF_VOLATILE:
                    vol.add(name)
    for name in a.omit:
        if items.pop(name, None) is None:
            sys.exit(f'--omit {name}: no such entry')
        if name in vol:
            print(f'warning: --omit {name}: a volatile entry (written in place on the phone): its contents are lost')
            vol.discard(name)
    for arg in a.files:
        name, _, rest = arg.partition('=')
        path, _, typ = rest.partition(':')
        items[name] = (TYPES[typ] if typ else guess_type(name), Path(path).read_bytes())
    # the loader's crash log (docs/STARBITE-HEALTH.md): on the UFS store; an imported one keeps its reports
    if (a.medium == 'ufs' and not a.no_crashlog) or a.crashlog:
        if CRASHLOG not in items or len(items[CRASHLOG][1]) != CL_BYTES:
            if CRASHLOG in items:
                print(f'{CRASHLOG}: imported entry is {len(items[CRASHLOG][1])} bytes, not {CL_BYTES}: started empty')
            items[CRASHLOG] = (TYPES['data'], bytes(CL_BYTES))
        vol.add(CRASHLOG)
    elif a.no_crashlog:
        items.pop(CRASHLOG, None)
        vol.discard(CRASHLOG)
    # --volatile NAME=BYTES: an entry an OS writes in place (Starbite OS's "sbos-state", docs/STARBITE-OS.md 6.5);
    # zeros when new, an imported one keeps its bytes (its size must match)
    for arg in a.volatile:
        name, _, size = arg.partition('=')
        try:
            size = int(size, 0)
        except ValueError:
            sys.exit(f'--volatile {arg}: NAME=BYTES')
        if not name or size <= 0 or size % BLOCK or size > 64 * 1048576:
            sys.exit(f'--volatile {arg}: a whole number of 4 KiB blocks, at most 64 MiB')
        if name in items and len(items[name][1]) != size:
            sys.exit(f'--volatile {name}: the imported entry is {len(items[name][1])} bytes, not {size}')
        if name not in items:
            items[name] = (TYPES['data'], bytes(size))
        vol.add(name)
    if len(items) > MAX_ENTRIES:
        sys.exit('too many entries')
    dirb, v1, data = bytearray(DIR_BLOCKS * BLOCK), bytearray(BLOCK), bytearray()
    blk, nv1 = DATA_BLOCK, 0
    rows = []
    for i, (name, (typ, blob)) in enumerate(items.items()):
        if not name or len(name) > 31:
            sys.exit(f'bad entry name {name!r}')
        crc = zlib.crc32(blob)
        fl = SF_VOLATILE if name in vol else 0
        struct.pack_into('<32sQQIHH8x', dirb, 64 * i, name.encode(), blk, len(blob), crc, typ, fl)
        nblk = (len(blob) + BLOCK - 1) // BLOCK
        if len(name) < 24 and blk + nblk <= V1_BLOCKS and nv1 < 63 and not fl:
            struct.pack_into('<24sQQII8x', v1, 64 + 64 * nv1, name.encode(), blk * BLOCK, len(blob), crc, 0)
            nv1 += 1
        rows.append((name, typ, blk, len(blob), crc))
        data += blob + bytes(-len(blob) % BLOCK)
        blk += nblk
    if blk > a.capacity:
        sys.exit(f'store needs {blk} blocks > capacity {a.capacity}')
    if a.medium == 'ufs' and blk > a.capacity - HEROLOG_BLOCKS:
        sys.exit(f'store needs {blk} blocks: past {a.capacity - HEROLOG_BLOCKS} it would overlap the USERDATA herolog ring')
    lay = SD_LAYOUTS.get(a.medium)
    if lay is not None and blk > a.capacity - SD_ROOT_TAIL_BLOCKS:
        sys.exit(f'store needs {blk} blocks: past {a.capacity - SD_ROOT_TAIL_BLOCKS} it would overlap the '
                 f'microSD root test range ({lay.tail.sdroot()})')
    if a.medium == 'sd64' and blk > lay.store_area_blocks:
        sys.exit(f'store needs {blk} blocks: past {lay.store_area_blocks} it would overlap the {lay.name} '
                 f'macOS Ventura root ({lay.root("mac13").sdroot()})')
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


def header(a):
    data = Path(a.img).read_bytes()[:2 * BLOCK]
    if data[BLOCK:BLOCK + 8] != b'SBSTORE2':
        sys.exit(f'{a.img}: no version 2 header')
    n, cap, used, _ = check_header(data[BLOCK:BLOCK + 52], a.img)
    print(f'{a.img}: {n} entries, used {used} of {cap} blocks')
    lay = next((L for L in SD_LAYOUTS.values() if cap == L.store_blocks), None)
    if lay is not None:
        tail = cap - SD_ROOT_TAIL_BLOCKS
        state = 'clear' if used <= tail else f'OVERLAPPED by {used - tail} blocks'
        print(f'microSD root tail (p2 blocks {tail}..{cap - 1}, {lay.tail.sdroot()}): {state}')
        area = lay.store_area_blocks
        state = 'clear' if used <= area else f'OVERLAPPED by {used - area} blocks'
        print(f'{lay.name} store area (p2 blocks 0..{area - 1}; the first root follows): {state}')


def listing(a):
    names = {v: k for k, v in TYPES.items()}
    for name, typ, blob, fl in read_store(a.img, with_flags=True):
        print(f'  {name:<24} {names.get(typ, "?") if typ is not None else "v1":<8} {len(blob):>10} crc32 {zlib.crc32(blob):08x}'
              + ('  volatile (written by the loader)' if fl & SF_VOLATILE else ''))


# ---- settings record (block 2; shim/starlte/sbfmt.h has the byte layout) ---------------------------
POST_LEVELS = {'full': 0, 'quick': 1, 'off': 2}
CRASH_MODES = {'on': 0, 'save': 1, 'off': 2}
CRASH_ACTIONS = {'menu': 0, 'recovery': 1, 'retry': 2}
WDT_MODES = {'entry': 0, 'off': 1, 'fixed': 2}
LOG_LEVELS = {'quiet': 0, 'normal': 1, 'verbose': 2, 'debug': 3}
SET_POST_SHIFT, SET_LOG_SHIFT = 4, 8            # flags bits 4-5 and 8-9 (shim/starlte/store.h)
SET_FIELDS = ('crash_mode', 'crash_detail', 'crash_action', 'crash_timeout', 'wdt_mode')   # bytes 104..108


def settings_decode(b):
    """dict of a valid 256-byte record (bytes 104.. read as their defaults when zero), else None."""
    if len(b) < 256 or b[:8] != b'SBSETTNG' or struct.unpack_from('<I', b, 8)[0] != 1 or \
            zlib.crc32(b[16:256]) != struct.unpack_from('<I', b, 12)[0]:
        return None
    seq, timeout, flags = struct.unpack_from('<III', b, 16)
    st = {'seq': seq, 'timeout': timeout, 'flags': flags,
          'keys': [b[o:o + 24].split(b'\0')[0].decode() for o in (32, 56, 80)]}
    st.update(zip(SET_FIELDS, b[104:109]))
    st['wdt_s'] = struct.unpack_from('<H', b, 110)[0]
    st['tail'] = bytes(b[112:256])
    return st


def settings_encode(st):
    b = bytearray(BLOCK)
    struct.pack_into('<8sII', b, 0, b'SBSETTNG', 1, 0)
    struct.pack_into('<IIII24s24s24s', b, 16, st['seq'], st['timeout'], st['flags'], 0, *(k.encode() for k in st['keys']))
    b[104:109] = bytes(st[f] for f in SET_FIELDS)
    struct.pack_into('<H', b, 110, st['wdt_s'])
    b[112:256] = st['tail']
    struct.pack_into('<I', b, 12, zlib.crc32(bytes(b[16:256])))
    return bytes(b)


def settings_text(st):
    def name(d, v):
        return next((k for k, x in d.items() if x == v), f'? ({v})')
    pl, ll = st['flags'] >> SET_POST_SHIFT & 3, st['flags'] >> SET_LOG_SHIFT & 3
    post = name(POST_LEVELS, pl) if pl <= 2 else 'full (3)'
    wdt = ('per entry' if st['wdt_mode'] == 0 else 'off' if st['wdt_mode'] == 1
           else f"{st['wdt_s']} s for every entry with a watchdog" if st['wdt_s'] else 'per entry')
    tmo = 60 if not st['crash_timeout'] else 10 * min(st['crash_timeout'], 60)
    k = st['keys']
    return (f"default {k[0] or '-'}, last {k[1] or '-'}, next {k[2] or '-'}, timeout {st['timeout']} s, card store at boot "
            f"{'on' if st['flags'] & SET_SD_STORE else 'off'}\n"
            f"POST {post}, log level {name(LOG_LEVELS, ll)}, crash detection "
            f"{name(CRASH_MODES, st['crash_mode'])}, crash screen detail {st['crash_detail'] or 1}, after a crash "
            f"{name(CRASH_ACTIONS, st['crash_action'])}, crash screen {tmo} s, watchdog {wdt}")


def settings(a):
    st = {'seq': 0, 'timeout': 0, 'flags': 0, 'keys': ['', '', ''], 'wdt_s': 0, 'tail': bytes(144)}
    st.update({f: 0 for f in SET_FIELDS})
    if a.frm:
        old = settings_decode(Path(a.frm).read_bytes()[:BLOCK])
        if old is not None:
            st = old
    if a.default is not None:
        st['keys'][0] = a.default
    if a.last is not None:
        st['keys'][1] = a.last
    if a.next is not None:
        st['keys'][2] = a.next
    if a.timeout is not None:
        st['timeout'] = a.timeout
    if a.sd is not None:
        st['flags'] = (st['flags'] | SET_SD_STORE) if a.sd == 'on' else (st['flags'] & ~SET_SD_STORE)
    if a.post is not None:
        st['flags'] = st['flags'] & ~(3 << SET_POST_SHIFT) | POST_LEVELS[a.post] << SET_POST_SHIFT
    if a.log is not None:
        st['flags'] = st['flags'] & ~(3 << SET_LOG_SHIFT) | LOG_LEVELS[a.log] << SET_LOG_SHIFT
    if a.crash is not None:
        st['crash_mode'] = CRASH_MODES[a.crash]
    if a.crash_detail is not None:
        st['crash_detail'] = a.crash_detail
    if a.crash_action is not None:
        st['crash_action'] = CRASH_ACTIONS[a.crash_action]
    if a.crash_timeout is not None:
        if not 10 <= a.crash_timeout <= 600:
            sys.exit('--crash-timeout: 10..600 s')
        st['crash_timeout'] = 0 if a.crash_timeout == 60 else (a.crash_timeout + 5) // 10
    if a.wdt is not None:
        if a.wdt in ('entry', 'off'):
            st['wdt_mode'], st['wdt_s'] = WDT_MODES[a.wdt], 0
        else:
            secs = int(a.wdt)
            if not 10 <= secs <= 82:
                sys.exit('--wdt: entry, off or 10..82 seconds')
            st['wdt_mode'], st['wdt_s'] = WDT_MODES['fixed'], secs
    if any(len(k) > 23 for k in st['keys']):
        sys.exit('keys are at most 23 characters')
    st['seq'] += 1
    Path(a.out).write_bytes(settings_encode(st))
    print(f'{a.out}: {settings_text(st)}')


# ---- the crash log (store entry "crashlog"; shim/starlte/sbfmt.h, docs/STARBITE-HEALTH.md) ----------
KINDS = ['NO OS WAS RUNNING', 'CLEAN RESTART', 'POWER ON', 'KERNEL PANIC', 'HANG (WATCHDOG)', 'HANG (OS TIMER)',
         'FORCED RESTART', 'FAULT (S-BOOT)', 'UNEXPLAINED RESET', 'WATCHDOG, NO OS', 'KERNEL OOPS']
RR_NAMES = {1: 'SPON (S-BOOT: FAULT)', 2: 'WPON WATCHDOG', 3: 'DPON (S-BOOT: DEBUG/UPLOAD)', 4: 'KPON KERNEL PANIC',
            5: 'MPON MANUAL RESET (KEYS)', 6: 'PPON (S-BOOT: FAULT)', 7: 'RPON REBOOT', 8: 'BPON', 9: 'NPON POWER ON',
            10: 'TPON'}
RST_BITS = {**{b: 'CLUSTER1_DBGRSTREQ' for b in range(0, 4)}, **{b: 'CLUSTER0_DBGRSTREQ' for b in range(4, 8)},
            **{b: 'CLUSTER1_WARMRSTREQ' for b in range(8, 12)}, **{b: 'CLUSTER0_WARMRSTREQ' for b in range(12, 16)},
            16: 'PINRESET', 19: 'CHUB_CPU_WDTRESET', 20: 'APM_WDTRESET', 21: 'APM_SYSRESET', 22: 'VTS_CPU_WDTRESET',
            23: 'CLUSTER1_WDTRESET', 24: 'CLUSTER0_WDTRESET', 25: 'AUD_CA7_WDTRESET', 26: 'SSS_WDTRESET', 28: 'WRESET',
            29: 'SWRESET'}
INFORM3_NAMES = {0: 'EMPTY', 0x12345670: 'NORMAL', 0x12345671: 'DOWNLOAD', 0x12345672: 'UPLOAD', 0x12345673: 'CHARGING',
                 0x12345674: 'RECOVERY', 0x12345675: 'FOTA', 0x1234567d: 'BOOTLOADER', 0xC8: 'PANIC (SEC_DEBUG)',
                 0x66: 'HARD RESET (SEC_DEBUG)', 0x85: 'POWER KEY LONG PRESS', 0xCAFEBABE: 'SEC_DEBUG INIT'}
SOURCES = ['REGISTERS ONLY', 'LINUX RAMOOPS DMESG RECORD', 'CONSOLE RING', 'SAMSUNG SEC_DEBUG PANIC STRING',
           'PMSG RING', 'S-BOOT RESET CODE']
METHOD_NAMES = {0: '-', 1: 'xnu', 2: 'linux', 3: 'android', 4: 'uefi', 5: 'bare'}
REG_NAMES = ['RST_STAT', 'INFORM2', 'INFORM3', 'PMU WDT disable', 'PMU WDT mask', 'S-Boot reset_reason',
             'S-Boot rst_stat', 'S-Boot pwronsrc', 'S-Boot pwroffsrc', 'sec_debug magic', 'WDT1 WTCON', 'WDT1 WTDAT',
             'WDT1 WTCNT', 'WDT0 WTCON', 'launch watchdog s', 'flags (launch | evidence << 16)']


def crashlog_decode(blob):
    """(header dict or None, [report dicts], newest first) of a 64 KiB crash log entry."""
    if len(blob) < CL_BYTES:
        sys.exit(f'crash log: {len(blob)} bytes, want {CL_BYTES}')
    h = blob[:CL_BLOCK]
    if h[:8] != b'SBCRASH1':
        return None, []
    if struct.unpack_from('<I', h, 8)[0] != 1 or zlib.crc32(h[16:1024]) != struct.unpack_from('<I', h, 12)[0]:
        sys.exit('crash log: header CRC mismatch')
    slots, ssize, nxt, count, boots, lseq, lstate = struct.unpack_from('<7I', h, 16)
    method, warmed, wdt_s = struct.unpack_from('<BBH', h, 44)
    head = {'slots': slots, 'next': nxt, 'count': count, 'boots': boots, 'launch_seq': lseq, 'running': lstate == 1,
            'method': method, 'wdt_armed': warmed, 'wdt_s': wdt_s,
            'key': h[48:72].split(b'\0')[0].decode(errors='replace'),
            'name': h[72:104].split(b'\0')[0].decode(errors='replace'),
            'launch_epoch': struct.unpack_from('<Q', h, 104)[0],
            'log_level': struct.unpack_from('<I', h, 112)[0], 'lflags': struct.unpack_from('<I', h, 116)[0],
            'seen': struct.unpack_from('<4I', h, 120)}
    if slots != CL_SLOTS or ssize != CL_BLOCK or nxt >= CL_SLOTS:
        sys.exit(f'crash log: unexpected header (slots {slots}, slot size {ssize}, next {nxt})')
    reps = []
    for k in range(min(count, CL_SLOTS)):
        slot = (nxt + CL_SLOTS - 1 - k) % CL_SLOTS
        b = blob[CL_BLOCK * (1 + slot):CL_BLOCK * (2 + slot)]
        if b[:8] != b'SBCRREP1':
            continue
        if zlib.crc32(b[16:]) != struct.unpack_from('<I', b, 12)[0]:
            reps.append({'slot': slot, 'bad': True})
            continue
        seq, kind, sev, meth, src = struct.unpack_from('<IBBBB', b, 16)
        det, lau, evt = struct.unpack_from('<3Q', b, 24)
        tlen, tlines = struct.unpack_from('<HH', b, 264)
        reps.append({'slot': slot, 'seq': seq, 'kind': kind, 'severity': sev, 'method': meth, 'source': src,
                     'detected': det, 'launched': lau, 'event': evt,
                     'key': b[48:72].split(b'\0')[0].decode(errors='replace'),
                     'name': b[72:104].split(b'\0')[0].decode(errors='replace'),
                     'reg': struct.unpack_from('<16I', b, 104),
                     'reason': b[168:264].split(b'\0')[0].decode(errors='replace'),
                     'text': b[272:272 + min(tlen, 3824)].decode(errors='replace'), 'lines': tlines})
    return head, reps


def utc(t):
    import datetime
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc).strftime('%Y-%m-%d %H:%M UTC') if t else 'unknown'


def report_text(r, level):
    if r.get('bad'):
        return f"--- slot {r['slot']}: CRC mismatch"
    reg = r['reg']
    out = [f"--- report #{r['seq']}: {r['name']} ({r['key']}, {METHOD_NAMES.get(r['method'], '?')})",
           f"kind: {KINDS[r['kind']] if r['kind'] < len(KINDS) else r['kind']} [{'crash' if r['severity'] >= 2 else 'warn'}]",
           f"reason: {r['reason']}",
           f"detected {utc(r['detected'])}, launched {utc(r['launched'])}"
           + (f", event (OS clock) {utc(r['event'])}" if r['event'] else ''),
           f"from: {SOURCES[r['source']] if r['source'] < len(SOURCES) else r['source']}",
           f"reset: {' '.join(sorted({RST_BITS[b] for b in RST_BITS if reg[0] >> b & 1})) or '-'}, INFORM3 "
           f"{INFORM3_NAMES.get(reg[2], hex(reg[2]))}, S-Boot {RR_NAMES.get(struct.unpack('<i', struct.pack('<I', reg[5]))[0], 'NONE')}"]
    if level >= 3:
        for i, n in enumerate(REG_NAMES):
            out.append(f'  {n:<32} 0x{reg[i]:08x}')
    if level >= 2:
        out.append(f"last {r['lines']} lines:")
        out.append(r['text'].rstrip('\n'))
    return '\n'.join(out)


def crashlog_cmd(a):
    import subprocess
    import tempfile
    if a.src == 'live':
        with tempfile.TemporaryDirectory() as t:
            for cmd in (['fastboot', 'oem', 'log', 'crashraw'], ['fastboot', 'get_staged', f'{t}/crashlog.bin']):
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
                if r.returncode:
                    sys.exit(f"{' '.join(cmd)}: {(r.stderr or r.stdout).strip()[-300:]} (the phone must be in the loader's "
                             'fastboot: hold Volume Up at boot, or `fastboot reboot bootloader`)')
            blob = Path(t, 'crashlog.bin').read_bytes()
    else:
        data = Path(a.src).read_bytes()
        if data[BLOCK:BLOCK + 8] == b'SBSTORE2':
            blob = {n: b for n, _, b in read_store(a.src)}.get(CRASHLOG)
            if blob is None:
                sys.exit(f'{a.src}: no {CRASHLOG} entry (a store built before 2026-11-06)')
        else:
            blob = data
    head, reps = crashlog_decode(blob)
    if head is None:
        print('crash log: empty (the loader writes its header at the first start)')
        return
    print(f"crash log: {head['count']} reports written, {head['boots']} launches judged; last launch #{head['launch_seq']} "
          f"{head['key'] or '-'} ({METHOD_NAMES.get(head['method'], '?')}) {utc(head['launch_epoch'])}"
          f"{' still running' if head['running'] else ''}"
          f"{', watchdog %d s' % head['wdt_s'] if head['wdt_armed'] else ''}, log level {head['log_level']}")
    for r in reps:
        print(report_text(r, a.level))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build')
    b.add_argument('out')
    b.add_argument('--capacity', type=int)
    b.add_argument('--medium', choices=sorted(MEDIA))
    b.add_argument('--catalog')
    b.add_argument('--import', dest='imp')
    b.add_argument('--drop', action='append', default=[], metavar='KEY')
    b.add_argument('--omit', action='append', default=[], metavar='NAME', help='leave an imported entry out')
    b.add_argument('--crashlog', action='store_true', help='add the crash log entry (automatic with --medium ufs)')
    b.add_argument('--no-crashlog', dest='no_crashlog', action='store_true', help='no crash log entry')
    b.add_argument('--volatile', action='append', default=[], metavar='NAME=BYTES',
                   help='an entry written in place by an OS (zeros when new; kept on --import), e.g. sbos-state=1048576')
    b.add_argument('files', nargs='*')
    l = sub.add_parser('list')
    l.add_argument('img')
    h = sub.add_parser('header')
    h.add_argument('img')
    c = sub.add_parser('catalog')
    c.add_argument('toml')
    c.add_argument('out')
    k = sub.add_parser('catalog-keys')
    k.add_argument('src')
    s = sub.add_parser('settings')
    s.add_argument('out')
    s.add_argument('--from', dest='frm')
    s.add_argument('--default')
    s.add_argument('--last')
    s.add_argument('--next')
    s.add_argument('--timeout', type=int)
    s.add_argument('--sd', choices=['on', 'off'])
    s.add_argument('--post', choices=list(POST_LEVELS))
    s.add_argument('--log', choices=list(LOG_LEVELS))
    s.add_argument('--crash', choices=list(CRASH_MODES))
    s.add_argument('--crash-detail', dest='crash_detail', type=int, choices=range(1, 4))
    s.add_argument('--crash-action', dest='crash_action', choices=list(CRASH_ACTIONS))
    s.add_argument('--crash-timeout', dest='crash_timeout', type=int, help='seconds, 10..600 (stored in tens)')
    s.add_argument('--wdt', help='entry (each catalog entry\'s own), off, or SECONDS (10..82) for every entry that has one')
    ss = sub.add_parser('settings-show')
    ss.add_argument('block')
    c2 = sub.add_parser('crashlog')
    c2.add_argument('src', help='store image, the raw 64 KiB entry, or "live" (the loader\'s fastboot)')
    c2.add_argument('--level', type=int, choices=range(1, 4), default=2)
    a = p.parse_args()
    if a.cmd == 'build':
        if a.capacity is None:
            if not a.medium:
                p.error('build needs --capacity or --medium')
            a.capacity = MEDIA[a.medium]
        build(a)
    elif a.cmd == 'list':
        listing(a)
    elif a.cmd == 'header':
        header(a)
    elif a.cmd == 'catalog':
        Path(a.out).write_bytes(pack_catalog(a.toml))
    elif a.cmd == 'catalog-keys':
        print('\n'.join(sorted(keys_of(a.src))))
    elif a.cmd == 'settings-show':
        b = Path(a.block).read_bytes()[:BLOCK]
        st = settings_decode(b)
        if st is None:
            sys.exit(f'{a.block}: no valid settings record (magic, version or CRC)')
        print(settings_text(st))
    elif a.cmd == 'crashlog':
        crashlog_cmd(a)
    else:
        settings(a)


if __name__ == '__main__':
    main()
