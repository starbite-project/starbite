#!/usr/bin/env python3
"""Undo HFS compression on iOS 26's card root through the card overlay (phone in TWRP; kernel 0406+).
    tools/ios26-decmpfs-fix.py SCAN.tsv OUTDIR [--prefix /System/Library/ExtensionKit ...] [--max-mib 30]
Our XNU has no decmpfs decompressors (AppleFSCompression is not in the kernel), so every compressed file on the
root ("tried to access a compressed file of unregistered type 12" in the kernel log) is unreadable on the phone.
For each compressed path in SCAN.tsv (tools/ios26-compressed-scan.py), filtered by --prefix, this reads the
com.apple.decmpfs xattr and the resource fork (read only), decompresses on the Mac (libcompression), checks the
size, and packs overlay rounds of at most --max-mib MiB and 120 files:
    rename:/path=/private/var/tmp/sbz-<n>     (the compressed original, kept)
    OUTDIR/files/<n>=/path:MODE:UID:GID       (a plain copy, same owner and mode)
OUTDIR/round-K.bin: one overlay per boot (tools/overlay/card-overlay.sh OUTDIR/round-K.bin, then boot);
OUTDIR/undo-K.bin: moves the copy to /private/var/tmp/sbz-<n>.new and the original back.
OUTDIR/plan.tsv lists every file; failures are listed, not packed."""
import argparse
import importlib.util
import os
import struct
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if not os.environ.get('HFS_IMAGE'):
    os.environ['HFS_CARD'] = '1'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


hr = load('hfsread', HERE / 'overlay' / 'hfs-read.py')
ia = load('ipswapfs', HERE / 'macos' / 'ipsw-apfs.py')      # decmpfs_chunk, DECMPFS (libcompression)
CHUNK = 64 << 10
MAX_FILES = 120             # 2 overlay entries each, the kernel takes 256


def decompress(x, rsrc):
    if len(x) < 16 or x[:4] != b'fpmc':
        raise IOError(f'bad decmpfs header {x[:16].hex()}')
    ctype, = struct.unpack_from('<I', x, 4)
    usize, = struct.unpack_from('<Q', x, 8)
    if ctype not in ia.DECMPFS:
        raise IOError(f'decmpfs type {ctype} not handled')
    kind, inline = ia.DECMPFS[ctype]
    if kind == 'none':
        return x[16:16 + usize], ctype
    if inline:
        return ia.decmpfs_chunk(kind, x[16:], usize), ctype
    if usize == 0:
        return b'', ctype
    n = (usize + CHUNK - 1) // CHUNK
    if ctype == 4:                      # zlib resource map: chunk table at 0x104, offsets relative to it
        count, = struct.unpack_from('<I', rsrc, 0x104)
        if count != n:
            raise IOError(f'zlib chunk count {count}, size says {n}')
        spans = [struct.unpack_from('<II', rsrc, 0x108 + 8 * i) for i in range(n)]
        spans = [(0x104 + o, 0x104 + o + ln) for o, ln in spans]
    else:                               # lzvn / lzfse / lzbitmap / raw: n + 1 LE u32 offsets from the start
        offs = struct.unpack_from(f'<{n + 1}I', rsrc, 0)
        spans = list(zip(offs, offs[1:]))
    out = bytearray()
    for i, (a, b) in enumerate(spans):
        out += ia.decmpfs_chunk(kind, rsrc[a:b], min(CHUNK, usize - i * CHUNK))
    if len(out) != usize:
        raise IOError(f'decompressed {len(out)} bytes, header says {usize}')
    return bytes(out), ctype


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('scan')
    ap.add_argument('outdir')
    ap.add_argument('--prefix', action='append', default=[])
    ap.add_argument('--max-mib', type=int, default=30)
    a = ap.parse_args()
    rows = [l.rstrip('\n').split('\t') for l in open(a.scan)][1:]
    paths = [r[0] for r in rows if not a.prefix or any(r[0].startswith(p) for p in a.prefix)]
    out = Path(a.outdir)
    (out / 'files').mkdir(parents=True, exist_ok=True)
    vol = hr.Volume()
    plan, rounds, cur, cur_bytes, fails = [], [], [], 0, 0
    for n, path in enumerate(paths):
        if ':' in path or '=' in path or len(path.encode()) >= 232:
            plan.append((path, 'skip', 'path not packable')); fails += 1
            continue
        try:
            rec, x, rsrc = vol.compressed_parts(path)
            data, ctype = decompress(x, rsrc)
        except (IOError, SystemExit, struct.error, ValueError) as e:
            plan.append((path, 'fail', str(e))); fails += 1
            continue
        hr.cache.clear()
        uid, gid = struct.unpack_from('>II', rec, 32)
        mode = struct.unpack_from('>H', rec, 42)[0] & 0o7777
        local = out / 'files' / str(n)
        local.write_bytes(data)
        if len(data) > a.max_mib << 20:
            plan.append((path, 'fail', f'{len(data)} bytes: bigger than one round')); fails += 1
            local.unlink()
            continue
        if cur and (cur_bytes + len(data) > a.max_mib << 20 or len(cur) >= MAX_FILES):
            rounds.append(cur); cur, cur_bytes = [], 0
        cur.append((n, path, local, mode, uid, gid))
        cur_bytes += len(data)
        plan.append((path, f'round {len(rounds) + 1}', f'type {ctype}, {len(data)} bytes, park /private/var/tmp/sbz-{n}'))
    if cur:
        rounds.append(cur)
    for k, batch in enumerate(rounds, 1):
        args, undo = [], []
        for n, path, local, mode, uid, gid in batch:
            args += [f'rename:{path}=/private/var/tmp/sbz-{n}', f'{local}={path}:{mode:o}:{uid}:{gid}']
            undo += [f'rename:{path}=/private/var/tmp/sbz-{n}.new', f'rename:/private/var/tmp/sbz-{n}={path}']
        for name, ops in ((f'round-{k}.bin', args), (f'undo-{k}.bin', undo)):
            r = subprocess.run([sys.executable, str(HERE / 'overlay' / 'sboverlay.py'), str(out / name)] + ops,
                               capture_output=True, text=True)
            if r.returncode:
                sys.exit(f'sboverlay {name}: {r.stderr or r.stdout}')
            print(r.stdout.strip().splitlines()[-1])
    with open(out / 'plan.tsv', 'w') as f:
        for row in plan:
            f.write('\t'.join(row) + '\n')
    print(f'{len(paths)} paths: {len(paths) - fails} packed in {len(rounds)} round(s), {fails} failed/skipped '
          f'-> {out}/plan.tsv')


if __name__ == '__main__':
    main()
