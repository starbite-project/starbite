#!/usr/bin/env python3
"""Extract one image's segments from iOS 26's dyld shared cache on the card, read only (phone in TWRP).
    tools/ios26-dsc.py OUTDIR NAME [NAME ...] [--segments __TEXT,__DATA_CONST]
NAME matches a substring of the image path (e.g. /ExtensionFoundation.framework/ExtensionFoundation). Reads only
the cache files' headers and mapping tables, the main file's image table, and the wanted image's segments (by
default __TEXT: code, C strings, ObjC method names), so nothing big is copied. Writes OUTDIR/<image>.<seg>.bin and
OUTDIR/<image>.json (vmaddr of each segment and section, so the bytes can be disassembled at their real addresses).
HFS_IMAGE is not supported (the cache lives only on the card)."""
import argparse
import importlib.util
import json
import os
import struct
import sys
from pathlib import Path

os.environ['HFS_CARD'] = '1'
_spec = importlib.util.spec_from_file_location('hfsread', Path(__file__).with_name('overlay') / 'hfs-read.py')
hr = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hr)
DIR = '/System/Library/Caches/com.apple.dyld'
MAIN = 'dyld_shared_cache_arm64e'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('outdir')
    ap.add_argument('names', nargs='+')
    ap.add_argument('--segments', default='__TEXT')
    a = ap.parse_args()
    out = Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    vol = hr.Volume()
    files = [n for n, k, _ in vol.listdir(DIR) if k == 'file' and n.startswith(MAIN)
             and not n.endswith(('.atlas', '.symbols', '.map'))]
    forks, maps = {}, []
    for n in sorted(files):
        rec = vol.file_record(f'{DIR}/{n}')
        fork = vol.fork(rec, 88, struct.unpack_from('>I', rec, 8)[0])
        forks[n] = fork
        h = vol.fork_read(fork, 0, 0x200)
        if not h.startswith(b'dyld_v1'):
            continue
        mo, mc = struct.unpack_from('<II', h, 0x10)
        mt = vol.fork_read(fork, mo, 32 * mc)
        for i in range(mc):
            addr, size, off, _, _ = struct.unpack_from('<QQQII', mt, 32 * i)
            maps.append((addr, size, n, off))
        hr.cache.clear()
    print(f'{len(forks)} cache files, {len(maps)} mappings', file=sys.stderr)

    def read(addr, n):
        for ma, ms, f, mo in maps:
            if ma <= addr and addr + n <= ma + ms:
                return vol.fork_read(forks[f], mo + addr - ma, n)
        raise SystemExit(f'address {addr:#x} (+{n:#x}) is in no mapping')

    mh = vol.fork_read(forks[MAIN], 0, 0x200)
    io, ic = struct.unpack_from('<II', mh, 0x1c0)
    if not ic:
        io, ic = struct.unpack_from('<II', mh, 0x18)
    table = vol.fork_read(forks[MAIN], io, 32 * ic)
    imgs = [struct.unpack_from('<QQQII', table, 32 * i) for i in range(ic)]
    # path strings: one read covering all of them
    lo = min(im[3] for im in imgs)
    hi = max(im[3] for im in imgs) + 512
    blob = vol.fork_read(forks[MAIN], lo, hi - lo)
    paths = [blob[im[3] - lo:].split(b'\0')[0].decode() for im in imgs]
    want = set(a.segments.split(','))
    found = 0
    for (addr, _, _, _, _), path in zip(imgs, paths):
        if not any(n in path for n in a.names):
            continue
        found += 1
        base = Path(path).name
        hdr = read(addr, 32)
        magic, _, _, _, ncmds, sizeofcmds = struct.unpack_from('<IiiIII', hdr, 0)
        if magic != 0xfeedfacf:
            print(f'{path}: bad Mach-O magic {magic:#x}', file=sys.stderr)
            continue
        lc = read(addr + 32, sizeofcmds)
        info = {'path': path, 'header': addr, 'segments': []}
        o = 0
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', lc, o)
            if cmd == 0x19:
                seg = lc[o + 8:o + 24].split(b'\0')[0].decode()
                vmaddr, vmsize = struct.unpack_from('<QQ', lc, o + 24)
                nsects, = struct.unpack_from('<I', lc, o + 64)
                sects = []
                for s in range(nsects):
                    so = o + 72 + 80 * s
                    sn = lc[so:so + 16].split(b'\0')[0].decode()
                    sa, ss = struct.unpack_from('<QQ', lc, so + 32)
                    sects.append({'name': sn, 'addr': sa, 'size': ss})
                info['segments'].append({'name': seg, 'vmaddr': vmaddr, 'vmsize': vmsize, 'sections': sects})
                if seg in want:
                    data = read(vmaddr, vmsize)
                    (out / f'{base}.{seg}.bin').write_bytes(data)
                    print(f'{path}: {seg} {vmaddr:#x} {vmsize:#x} -> {out}/{base}.{seg}.bin')
                    hr.cache.clear()
            o += size
        (out / f'{base}.json').write_text(json.dumps(info, indent=1))
    if not found:
        sys.exit('no image matched')


if __name__ == '__main__':
    main()
