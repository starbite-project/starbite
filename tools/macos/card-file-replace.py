#!/usr/bin/env python3
"""Replace one file's bytes in place inside the macOS root on the S9's microSD card, from TWRP over adb, writing
nothing but that file's own data blocks (2026-11-12, S2g: the PanVK dylib had to change while the card's root had
diverged from every image on the Mac, so no chunk-level reflash could be used).

  tools/macos/card-file-replace.py ROOT_PATH NEW_FILE --expect-old SHA256 [--start 182888448] [--count 33554432]
                                   [--dry-run]

Checks before anything is written (any failure: nothing written):
  - TWRP, the 128 GB card (249999360 sectors);
  - the HFS+/HFSX volume at --start (tools/overlay/hfs-read.py reads it over adb): ROOT_PATH's catalog record, its
    data fork's extents (overflow extents too), not HFS-compressed;
  - the file's current bytes on the card hash to --expect-old (the file the Mac staged, untouched since);
  - NEW_FILE has exactly the fork's logical size, so no catalog, extents or bitmap change is needed (the journal
    covers metadata only: data blocks are never replayed over);
  - every extent lies inside [--start, --start + --count) (the sdroot= range).
Writes: per extent, the new bytes (the slack after the logical end keeps its old bytes) pushed to /tmp, checked by
sha256 on the phone, dd'd with conv=fsync; then the file is read back from the card and must hash to NEW_FILE's sha256.
"""
import argparse
import hashlib
import importlib.util
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CARD_SECTORS = 249999360


def adb(*args, **kw):
    return subprocess.run(['adb', *args], capture_output=True, check=True, **kw)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('path')
    ap.add_argument('new_file')
    ap.add_argument('--expect-old', required=True)
    ap.add_argument('--start', type=int, default=182888448)
    ap.add_argument('--count', type=int, default=33554432)
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()

    if adb('get-state').stdout.decode().strip() != 'recovery':
        sys.exit('phone not in TWRP')
    size = adb('shell', 'cat /sys/block/mmcblk0/size').stdout.decode().strip()
    if size != str(CARD_SECTORS):
        sys.exit(f'card has {size} sectors, not the 128 GB card ({CARD_SECTORS})')

    os.environ['HFS_CARD'] = '1'
    spec = importlib.util.spec_from_file_location('hfsread', ROOT / 'tools/overlay/hfs-read.py')
    hr = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(hr)
    if hr.BASE != a.start * 512:
        hr.BASE = a.start * 512
    vol = hr.Volume()
    if vol.block != 4096:
        sys.exit(f'volume block size {vol.block}, not 4096')
    rec = vol.file_record(a.path)
    fid = int.from_bytes(rec[8:12], 'big')
    bsd_flags = rec[41]                      # ownerFlags byte of HFSPlusBSDInfo (UF_COMPRESSED = 0x20)
    if bsd_flags & 0x20:
        sys.exit(f'{a.path}: HFS-compressed: not replaceable in place')
    logical, extents = vol.fork(rec, 88, fid)
    new = Path(a.new_file).read_bytes()
    print(f'{a.path}: file id {fid}, {logical} bytes in {len(extents)} extent(s): '
          + ', '.join(f'{s}+{c}' for s, c in extents))
    if len(new) != logical:
        sys.exit(f'{a.new_file} has {len(new)} bytes, the file on the card {logical}: sizes must match exactly')
    first_card_block = a.start // 8
    for s, c in extents:
        lo, hi = a.start + s * 8, a.start + (s + c) * 8
        if lo < a.start or hi > a.start + a.count:
            sys.exit(f'extent {s}+{c} (sectors {lo}..{hi}) is outside the range {a.start}+{a.count}')
    old = vol.fork_read((logical, extents), 0, logical)
    got = hashlib.sha256(old).hexdigest()
    if got != a.expect_old:
        sys.exit(f'the file on the card hashes to {got}, not --expect-old {a.expect_old}: refusing')
    want = hashlib.sha256(new).hexdigest()
    print(f'card: old sha256 {got} (as expected); new {want}')
    if a.dry_run:
        print('dry run: nothing written')
        return

    done = 0
    with tempfile.TemporaryDirectory() as tmp:
        for s, c in extents:
            nbytes = c * 4096
            alloc = vol.fork_read((nbytes, [(s, c)]), 0, nbytes)       # this extent as it is now (slack kept)
            take = min(nbytes, logical - done)
            buf = new[done:done + take] + alloc[take:]
            local = Path(tmp) / 'chunk'
            local.write_bytes(buf)
            h = hashlib.sha256(buf).hexdigest()
            adb('push', str(local), '/tmp/sbrepl.bin')
            seek = first_card_block + s
            out = adb('shell', f'[ "$(sha256sum /tmp/sbrepl.bin | cut -d" " -f1)" = {h} ] || {{ echo BADPUSH; exit 0; }}; '
                      f'dd if=/tmp/sbrepl.bin of=/dev/block/mmcblk0 bs=4096 seek={seek} count={c} conv=fsync,notrunc '
                      f'2>/dev/null || echo BADWRITE; sync; rm -f /tmp/sbrepl.bin').stdout.decode()
            if 'BAD' in out:
                sys.exit(f'extent {s}+{c}: {out.strip()} (written so far: {done} bytes)')
            done += take
            print(f'  extent {s}+{c}: {take} bytes written')
    hr.cache.clear()
    back = hashlib.sha256(vol.fork_read((logical, extents), 0, logical)).hexdigest()
    if back != want:
        sys.exit(f'read back {back}, expected {want}: CHECK THE CARD')
    print(f'done: {a.path} on the card now hashes to {want} (read back)')


if __name__ == '__main__':
    main()
